// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/core/quic_dispatcher.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "quiche/quic/core/chlo_extractor.h"
#include "quiche/quic/core/crypto/crypto_protocol.h"
#include "quiche/quic/core/crypto/quic_random.h"
#include "quiche/quic/core/quic_connection_id.h"
#include "quiche/quic/core/quic_error_codes.h"
#include "quiche/quic/core/quic_session.h"
#include "quiche/quic/core/quic_time_wait_list_manager.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/core/tls_chlo_extractor.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"
#include "quiche/quic/platform/api/quic_flag_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/platform/api/quic_stack_trace.h"
#include "quiche/common/quiche_text_utils.h"

namespace quic {

using BufferedPacket = QuicBufferedPacketStore::BufferedPacket;
using BufferedPacketList = QuicBufferedPacketStore::BufferedPacketList;
using EnqueuePacketResult = QuicBufferedPacketStore::EnqueuePacketResult;

namespace {

// Minimal INITIAL packet length sent by clients is 1200.
const QuicPacketLength kMinClientInitialPacketLength = 1200;

// An alarm that informs the QuicDispatcher to delete old sessions.
class DeleteSessionsAlarm : public QuicAlarm::DelegateWithoutContext {
 public:
  explicit DeleteSessionsAlarm(QuicDispatcher* dispatcher)
      : dispatcher_(dispatcher) {}
  DeleteSessionsAlarm(const DeleteSessionsAlarm&) = delete;
  DeleteSessionsAlarm& operator=(const DeleteSessionsAlarm&) = delete;

  void OnAlarm() override { dispatcher_->DeleteSessions(); }

 private:
  // Not owned.
  QuicDispatcher* dispatcher_;
};

// An alarm that informs the QuicDispatcher to clear
// recent_stateless_reset_addresses_.
class ClearStatelessResetAddressesAlarm
    : public QuicAlarm::DelegateWithoutContext {
 public:
  explicit ClearStatelessResetAddressesAlarm(QuicDispatcher* dispatcher)
      : dispatcher_(dispatcher) {}
  ClearStatelessResetAddressesAlarm(const DeleteSessionsAlarm&) = delete;
  ClearStatelessResetAddressesAlarm& operator=(const DeleteSessionsAlarm&) =
      delete;

  void OnAlarm() override { dispatcher_->ClearStatelessResetAddresses(); }

 private:
  // Not owned.
  QuicDispatcher* dispatcher_;
};

// Collects packets serialized by a QuicPacketCreator in order
// to be handed off to the time wait list manager.
class PacketCollector : public QuicPacketCreator::DelegateInterface,
                        public QuicStreamFrameDataProducer {
 public:
  explicit PacketCollector(quiche::QuicheBufferAllocator* allocator)
      : send_buffer_(allocator) {}
  ~PacketCollector() override = default;

  // QuicPacketCreator::DelegateInterface methods:
  void OnSerializedPacket(SerializedPacket serialized_packet) override {
    // Make a copy of the serialized packet to send later.
    packets_.emplace_back(
        new QuicEncryptedPacket(CopyBuffer(serialized_packet),
                                serialized_packet.encrypted_length, true));
  }

  QuicPacketBuffer GetPacketBuffer() override {
    // Let QuicPacketCreator to serialize packets on stack buffer.
    return {nullptr, nullptr};
  }

  void OnUnrecoverableError(QuicErrorCode /*error*/,
                            const std::string& /*error_details*/) override {}

  bool ShouldGeneratePacket(HasRetransmittableData /*retransmittable*/,
                            IsHandshake /*handshake*/) override {
    QUICHE_DCHECK(false);
    return true;
  }

  const QuicFrames MaybeBundleAckOpportunistically() override {
    QUICHE_DCHECK(false);
    return {};
  }

  SerializedPacketFate GetSerializedPacketFate(
      bool /*is_mtu_discovery*/,
      EncryptionLevel /*encryption_level*/) override {
    return SEND_TO_WRITER;
  }

  // QuicStreamFrameDataProducer
  WriteStreamDataResult WriteStreamData(QuicStreamId /*id*/,
                                        QuicStreamOffset offset,
                                        QuicByteCount data_length,
                                        QuicDataWriter* writer) override {
    if (send_buffer_.WriteStreamData(offset, data_length, writer)) {
      return WRITE_SUCCESS;
    }
    return WRITE_FAILED;
  }
  bool WriteCryptoData(EncryptionLevel /*level*/, QuicStreamOffset offset,
                       QuicByteCount data_length,
                       QuicDataWriter* writer) override {
    return send_buffer_.WriteStreamData(offset, data_length, writer);
  }

  std::vector<std::unique_ptr<QuicEncryptedPacket>>* packets() {
    return &packets_;
  }

 private:
  std::vector<std::unique_ptr<QuicEncryptedPacket>> packets_;
  // This is only needed until the packets are encrypted. Once packets are
  // encrypted, the stream data is no longer required.
  QuicStreamSendBuffer send_buffer_;
};

// Helper for statelessly closing connections by generating the
// correct termination packets and adding the connection to the time wait
// list manager.
class StatelessConnectionTerminator {
 public:
  StatelessConnectionTerminator(QuicConnectionId server_connection_id,
                                QuicConnectionId original_server_connection_id,
                                const ParsedQuicVersion version,
                                QuicConnectionHelperInterface* helper,
                                QuicTimeWaitListManager* time_wait_list_manager)
      : server_connection_id_(server_connection_id),
        framer_(ParsedQuicVersionVector{version},
                /*unused*/ QuicTime::Zero(), Perspective::IS_SERVER,
                /*unused*/ kQuicDefaultConnectionIdLength),
        collector_(helper->GetStreamSendBufferAllocator()),
        creator_(server_connection_id, &framer_, &collector_),
        time_wait_list_manager_(time_wait_list_manager) {
    framer_.set_data_producer(&collector_);
    // Always set encrypter with original_server_connection_id.
    framer_.SetInitialObfuscators(original_server_connection_id);
  }

  ~StatelessConnectionTerminator() {
    // Clear framer's producer.
    framer_.set_data_producer(nullptr);
  }

  // Generates a packet containing a CONNECTION_CLOSE frame specifying
  // |error_code| and |error_details| and add the connection to time wait.
  void CloseConnection(QuicErrorCode error_code,
                       const std::string& error_details, bool ietf_quic,
                       std::vector<QuicConnectionId> active_connection_ids) {
    SerializeConnectionClosePacket(error_code, error_details);

    time_wait_list_manager_->AddConnectionIdToTimeWait(
        QuicTimeWaitListManager::SEND_TERMINATION_PACKETS,
        TimeWaitConnectionInfo(ietf_quic, collector_.packets(),
                               std::move(active_connection_ids),
                               /*srtt=*/QuicTime::Delta::Zero()));
  }

 private:
  void SerializeConnectionClosePacket(QuicErrorCode error_code,
                                      const std::string& error_details) {
    QuicConnectionCloseFrame* frame =
        new QuicConnectionCloseFrame(framer_.transport_version(), error_code,
                                     NO_IETF_QUIC_ERROR, error_details,
                                     /*transport_close_frame_type=*/0);

    if (!creator_.AddFrame(QuicFrame(frame), NOT_RETRANSMISSION)) {
      QUIC_BUG(quic_bug_10287_1) << "Unable to add frame to an empty packet";
      delete frame;
      return;
    }
    creator_.FlushCurrentPacket();
    QUICHE_DCHECK_EQ(1u, collector_.packets()->size());
  }

  QuicConnectionId server_connection_id_;
  QuicFramer framer_;
  // Set as the visitor of |creator_| to collect any generated packets.
  PacketCollector collector_;
  QuicPacketCreator creator_;
  QuicTimeWaitListManager* time_wait_list_manager_;
};

// Class which extracts the ALPN and SNI from a QUIC_CRYPTO CHLO packet.
class ChloAlpnSniExtractor : public ChloExtractor::Delegate {
 public:
  void OnChlo(QuicTransportVersion version,
              QuicConnectionId /*server_connection_id*/,
              const CryptoHandshakeMessage& chlo) override {
    absl::string_view alpn_value;
    if (chlo.GetStringPiece(kALPN, &alpn_value)) {
      alpn_ = std::string(alpn_value);
    }
    absl::string_view sni;
    if (chlo.GetStringPiece(quic::kSNI, &sni)) {
      sni_ = std::string(sni);
    }
    absl::string_view uaid_value;
    if (chlo.GetStringPiece(quic::kUAID, &uaid_value)) {
      uaid_ = std::string(uaid_value);
    }
    if (version == LegacyVersionForEncapsulation().transport_version) {
      absl::string_view qlve_value;
      if (chlo.GetStringPiece(kQLVE, &qlve_value)) {
        legacy_version_encapsulation_inner_packet_ = std::string(qlve_value);
      }
    }
  }

  std::string&& ConsumeAlpn() { return std::move(alpn_); }

  std::string&& ConsumeSni() { return std::move(sni_); }

  std::string&& ConsumeUaid() { return std::move(uaid_); }

  std::string&& ConsumeLegacyVersionEncapsulationInnerPacket() {
    return std::move(legacy_version_encapsulation_inner_packet_);
  }

 private:
  std::string alpn_;
  std::string sni_;
  std::string uaid_;
  std::string legacy_version_encapsulation_inner_packet_;
};

bool MaybeHandleLegacyVersionEncapsulation(
    QuicDispatcher* dispatcher,
    std::string legacy_version_encapsulation_inner_packet,
    const ReceivedPacketInfo& packet_info) {
  QUICHE_DCHECK(!GetQuicRestartFlag(quic_disable_legacy_version_encapsulation));
  if (legacy_version_encapsulation_inner_packet.empty()) {
    // This CHLO did not contain the Legacy Version Encapsulation tag.
    return false;
  }
  PacketHeaderFormat format;
  QuicLongHeaderType long_packet_type;
  bool version_present;
  bool has_length_prefix;
  QuicVersionLabel version_label;
  ParsedQuicVersion parsed_version = ParsedQuicVersion::Unsupported();
  QuicConnectionId destination_connection_id, source_connection_id;
  absl::optional<absl::string_view> retry_token;
  std::string detailed_error;
  const QuicErrorCode error = QuicFramer::ParsePublicHeaderDispatcher(
      QuicEncryptedPacket(legacy_version_encapsulation_inner_packet.data(),
                          legacy_version_encapsulation_inner_packet.length()),
      kQuicDefaultConnectionIdLength, &format, &long_packet_type,
      &version_present, &has_length_prefix, &version_label, &parsed_version,
      &destination_connection_id, &source_connection_id, &retry_token,
      &detailed_error);
  if (error != QUIC_NO_ERROR) {
    QUIC_DLOG(ERROR)
        << "Failed to parse Legacy Version Encapsulation inner packet:"
        << detailed_error;
    return false;
  }
  if (destination_connection_id != packet_info.destination_connection_id) {
    // We enforce that the inner and outer connection IDs match to make sure
    // this never impacts routing of packets.
    QUIC_DLOG(ERROR) << "Ignoring Legacy Version Encapsulation packet "
                        "with mismatched connection ID "
                     << destination_connection_id << " vs "
                     << packet_info.destination_connection_id;
    return false;
  }
  if (legacy_version_encapsulation_inner_packet.length() >=
      packet_info.packet.length()) {
    QUIC_BUG(quic_bug_10287_2)
        << "Inner packet cannot be larger than outer "
        << legacy_version_encapsulation_inner_packet.length() << " vs "
        << packet_info.packet.length();
    return false;
  }

  QUIC_DVLOG(1) << "Extracted a Legacy Version Encapsulation "
                << legacy_version_encapsulation_inner_packet.length()
                << " byte packet of version " << parsed_version;

  // Append zeroes to the end of the packet. This will ensure that
  // we use the right number of bytes for calculating anti-amplification
  // limits. Note that this only works for long headers of versions that carry
  // long header lengths, since they'll ignore any trailing zeroes. We still
  // do this for all packets to ensure version negotiation works.
  legacy_version_encapsulation_inner_packet.append(
      packet_info.packet.length() -
          legacy_version_encapsulation_inner_packet.length(),
      0x00);

  // Process the inner packet as if it had been received by itself.
  QuicReceivedPacket received_encapsulated_packet(
      legacy_version_encapsulation_inner_packet.data(),
      legacy_version_encapsulation_inner_packet.length(),
      packet_info.packet.receipt_time());
  dispatcher->ProcessPacket(packet_info.self_address, packet_info.peer_address,
                            received_encapsulated_packet);
  QUIC_CODE_COUNT(quic_legacy_version_encapsulation_decapsulated);
  return true;
}

}  // namespace

QuicDispatcher::QuicDispatcher(
    const QuicConfig* config, const QuicCryptoServerConfig* crypto_config,
    QuicVersionManager* version_manager,
    std::unique_ptr<QuicConnectionHelperInterface> helper,
    std::unique_ptr<QuicCryptoServerStreamBase::Helper> session_helper,
    std::unique_ptr<QuicAlarmFactory> alarm_factory,
    uint8_t expected_server_connection_id_length,
    ConnectionIdGeneratorInterface& connection_id_generator)
    : config_(config),
      crypto_config_(crypto_config),
      compressed_certs_cache_(
          QuicCompressedCertsCache::kQuicCompressedCertsCacheSize),
      helper_(std::move(helper)),
      session_helper_(std::move(session_helper)),
      alarm_factory_(std::move(alarm_factory)),
      delete_sessions_alarm_(
          alarm_factory_->CreateAlarm(new DeleteSessionsAlarm(this))),
      buffered_packets_(this, helper_->GetClock(), alarm_factory_.get()),
      version_manager_(version_manager),
      last_error_(QUIC_NO_ERROR),
      new_sessions_allowed_per_event_loop_(0u),
      accept_new_connections_(true),
      allow_short_initial_server_connection_ids_(false),
      expected_server_connection_id_length_(
          expected_server_connection_id_length),
      clear_stateless_reset_addresses_alarm_(alarm_factory_->CreateAlarm(
          new ClearStatelessResetAddressesAlarm(this))),
      should_update_expected_server_connection_id_length_(false),
      connection_id_generator_(connection_id_generator) {
  QUIC_BUG_IF(quic_bug_12724_1, GetSupportedVersions().empty())
      << "Trying to create dispatcher without any supported versions";
  QUIC_DLOG(INFO) << "Created QuicDispatcher with versions: "
                  << ParsedQuicVersionVectorToString(GetSupportedVersions());
  if (send_connection_close_for_tls_alerts_) {
    QUIC_RESTART_FLAG_COUNT_N(
        quic_dispatcher_send_connection_close_for_tls_alerts, 1, 3);
  }
}

QuicDispatcher::~QuicDispatcher() {
  if (delete_sessions_alarm_ != nullptr) {
    delete_sessions_alarm_->PermanentCancel();
  }
  if (clear_stateless_reset_addresses_alarm_ != nullptr) {
    clear_stateless_reset_addresses_alarm_->PermanentCancel();
  }
  reference_counted_session_map_.clear();
  closed_session_list_.clear();
  num_sessions_in_session_map_ = 0;
}

void QuicDispatcher::InitializeWithWriter(QuicPacketWriter* writer) {
  QUICHE_DCHECK(writer_ == nullptr);
  writer_.reset(writer);
  time_wait_list_manager_.reset(CreateQuicTimeWaitListManager());
}

void QuicDispatcher::ProcessPacket(const QuicSocketAddress& self_address,
                                   const QuicSocketAddress& peer_address,
                                   const QuicReceivedPacket& packet) {
  QUIC_DVLOG(2) << "Dispatcher received encrypted " << packet.length()
                << " bytes:" << std::endl
                << quiche::QuicheTextUtils::HexDump(
                       absl::string_view(packet.data(), packet.length()));
  ReceivedPacketInfo packet_info(self_address, peer_address, packet);
  std::string detailed_error;
  const QuicErrorCode error = QuicFramer::ParsePublicHeaderDispatcher(
      packet, expected_server_connection_id_length_, &packet_info.form,
      &packet_info.long_packet_type, &packet_info.version_flag,
      &packet_info.use_length_prefix, &packet_info.version_label,
      &packet_info.version, &packet_info.destination_connection_id,
      &packet_info.source_connection_id, &packet_info.retry_token,
      &detailed_error);
  if (error != QUIC_NO_ERROR) {
    // Packet has framing error.
    SetLastError(error);
    QUIC_DLOG(ERROR) << detailed_error;
    return;
  }
  if (packet_info.destination_connection_id.length() !=
          expected_server_connection_id_length_ &&
      !should_update_expected_server_connection_id_length_ &&
      packet_info.version.IsKnown() &&
      !packet_info.version.AllowsVariableLengthConnectionIds()) {
    SetLastError(QUIC_INVALID_PACKET_HEADER);
    QUIC_DLOG(ERROR) << "Invalid Connection Id Length";
    return;
  }

  if (packet_info.version_flag && IsSupportedVersion(packet_info.version)) {
    if (!QuicUtils::IsConnectionIdValidForVersion(
            packet_info.destination_connection_id,
            packet_info.version.transport_version)) {
      SetLastError(QUIC_INVALID_PACKET_HEADER);
      QUIC_DLOG(ERROR)
          << "Invalid destination connection ID length for version";
      return;
    }
    if (packet_info.version.SupportsClientConnectionIds() &&
        !QuicUtils::IsConnectionIdValidForVersion(
            packet_info.source_connection_id,
            packet_info.version.transport_version)) {
      SetLastError(QUIC_INVALID_PACKET_HEADER);
      QUIC_DLOG(ERROR) << "Invalid source connection ID length for version";
      return;
    }
  }

  if (should_update_expected_server_connection_id_length_) {
    expected_server_connection_id_length_ =
        packet_info.destination_connection_id.length();
  }

  if (MaybeDispatchPacket(packet_info)) {
    // Packet has been dropped or successfully dispatched, stop processing.
    return;
  }
  ProcessHeader(&packet_info);
}

absl::optional<QuicConnectionId> QuicDispatcher::MaybeReplaceServerConnectionId(
    const QuicConnectionId& server_connection_id,
    const ParsedQuicVersion& version) {
  if (GetQuicRestartFlag(quic_abstract_connection_id_generator)) {
    QUIC_RESTART_FLAG_COUNT(quic_abstract_connection_id_generator);
    return connection_id_generator_.MaybeReplaceConnectionId(
        server_connection_id, version);
  }
  const uint8_t server_connection_id_length = server_connection_id.length();
  if (server_connection_id_length == expected_server_connection_id_length_) {
    return absl::optional<QuicConnectionId>();
  }
  QUICHE_DCHECK(version.AllowsVariableLengthConnectionIds());
  QuicConnectionId new_connection_id;
  if (server_connection_id_length < expected_server_connection_id_length_) {
    new_connection_id = ReplaceShortServerConnectionId(
        version, server_connection_id, expected_server_connection_id_length_);
    // Verify that ReplaceShortServerConnectionId is deterministic.
    QUICHE_DCHECK_EQ(
        new_connection_id,
        ReplaceShortServerConnectionId(version, server_connection_id,
                                       expected_server_connection_id_length_));
  } else {
    new_connection_id = ReplaceLongServerConnectionId(
        version, server_connection_id, expected_server_connection_id_length_);
    // Verify that ReplaceLongServerConnectionId is deterministic.
    QUICHE_DCHECK_EQ(
        new_connection_id,
        ReplaceLongServerConnectionId(version, server_connection_id,
                                      expected_server_connection_id_length_));
  }
  QUICHE_DCHECK_EQ(expected_server_connection_id_length_,
                   new_connection_id.length());

  QUIC_DLOG(INFO) << "Replacing incoming connection ID " << server_connection_id
                  << " with " << new_connection_id;
  return new_connection_id;
}

QuicConnectionId QuicDispatcher::ReplaceShortServerConnectionId(
    const ParsedQuicVersion& /*version*/,
    const QuicConnectionId& server_connection_id,
    uint8_t expected_server_connection_id_length) const {
  QUICHE_DCHECK_LT(server_connection_id.length(),
                   expected_server_connection_id_length);
  return QuicUtils::CreateReplacementConnectionId(
      server_connection_id, expected_server_connection_id_length);
}

QuicConnectionId QuicDispatcher::ReplaceLongServerConnectionId(
    const ParsedQuicVersion& /*version*/,
    const QuicConnectionId& server_connection_id,
    uint8_t expected_server_connection_id_length) const {
  QUICHE_DCHECK_GT(server_connection_id.length(),
                   expected_server_connection_id_length);
  return QuicUtils::CreateReplacementConnectionId(
      server_connection_id, expected_server_connection_id_length);
}

namespace {
constexpr bool IsSourceUdpPortBlocked(uint16_t port) {
  // These UDP source ports have been observed in large scale denial of service
  // attacks and are not expected to ever carry user traffic, they are therefore
  // blocked as a safety measure. See draft-ietf-quic-applicability for details.
  constexpr uint16_t blocked_ports[] = {
      0,      // We cannot send to port 0 so drop that source port.
      17,     // Quote of the Day, can loop with QUIC.
      19,     // Chargen, can loop with QUIC.
      53,     // DNS, vulnerable to reflection attacks.
      111,    // Portmap.
      123,    // NTP, vulnerable to reflection attacks.
      137,    // NETBIOS Name Service,
      138,    // NETBIOS Datagram Service
      161,    // SNMP.
      389,    // CLDAP.
      500,    // IKE, can loop with QUIC.
      1900,   // SSDP, vulnerable to reflection attacks.
      3702,   // WS-Discovery, vulnerable to reflection attacks.
      5353,   // mDNS, vulnerable to reflection attacks.
      5355,   // LLMNR, vulnerable to reflection attacks.
      11211,  // memcache, vulnerable to reflection attacks.
              // This list MUST be sorted in increasing order.
  };
  constexpr size_t num_blocked_ports = ABSL_ARRAYSIZE(blocked_ports);
  constexpr uint16_t highest_blocked_port =
      blocked_ports[num_blocked_ports - 1];
  if (ABSL_PREDICT_TRUE(port > highest_blocked_port)) {
    // Early-return to skip comparisons for the majority of traffic.
    return false;
  }
  for (size_t i = 0; i < num_blocked_ports; i++) {
    if (port == blocked_ports[i]) {
      return true;
    }
  }
  return false;
}
}  // namespace

bool QuicDispatcher::MaybeDispatchPacket(
    const ReceivedPacketInfo& packet_info) {
  if (IsSourceUdpPortBlocked(packet_info.peer_address.port())) {
    // Silently drop the received packet.
    QUIC_CODE_COUNT(quic_dropped_blocked_port);
    return true;
  }

  QuicConnectionId server_connection_id = packet_info.destination_connection_id;

  // The IETF spec requires the client to generate an initial server
  // connection ID that is at least 64 bits long. After that initial
  // connection ID, the dispatcher picks a new one of its expected length.
  // Therefore we should never receive a connection ID that is smaller
  // than 64 bits and smaller than what we expect. Unless the version is
  // unknown, in which case we allow short connection IDs for version
  // negotiation because that version could allow those.
  if (packet_info.version_flag && packet_info.version.IsKnown() &&
      server_connection_id.length() < kQuicMinimumInitialConnectionIdLength &&
      server_connection_id.length() < expected_server_connection_id_length_ &&
      !allow_short_initial_server_connection_ids_) {
    QUICHE_DCHECK(packet_info.version_flag);
    QUICHE_DCHECK(packet_info.version.AllowsVariableLengthConnectionIds());
    QUIC_DLOG(INFO) << "Packet with short destination connection ID "
                    << server_connection_id << " expected "
                    << static_cast<int>(expected_server_connection_id_length_);
    // Drop the packet silently.
    QUIC_CODE_COUNT(quic_dropped_invalid_small_initial_connection_id);
    return true;
  }

  if (packet_info.version_flag && packet_info.version.IsKnown() &&
      !QuicUtils::IsConnectionIdLengthValidForVersion(
          server_connection_id.length(),
          packet_info.version.transport_version)) {
    QUIC_DLOG(INFO) << "Packet with destination connection ID "
                    << server_connection_id << " is invalid with version "
                    << packet_info.version;
    // Drop the packet silently.
    QUIC_CODE_COUNT(quic_dropped_invalid_initial_connection_id);
    return true;
  }

  // Packets with connection IDs for active connections are processed
  // immediately.
  auto it = reference_counted_session_map_.find(server_connection_id);
  if (it != reference_counted_session_map_.end()) {
    QUICHE_DCHECK(!buffered_packets_.HasBufferedPackets(server_connection_id));
    if (packet_info.version_flag &&
        packet_info.version != it->second->version() &&
        packet_info.version == LegacyVersionForEncapsulation()) {
      // This packet is using the Legacy Version Encapsulation version but the
      // corresponding session isn't, attempt extraction of inner packet.
      if (GetQuicRestartFlag(quic_disable_legacy_version_encapsulation)) {
        QUIC_CODE_COUNT(
            quic_disable_legacy_version_encapsulation_dispatch_packet);
      } else {
        ChloAlpnSniExtractor alpn_extractor;
        if (ChloExtractor::Extract(packet_info.packet, packet_info.version,
                                   config_->create_session_tag_indicators(),
                                   &alpn_extractor,
                                   server_connection_id.length())) {
          if (MaybeHandleLegacyVersionEncapsulation(
                  this,
                  alpn_extractor.ConsumeLegacyVersionEncapsulationInnerPacket(),
                  packet_info)) {
            return true;
          }
        }
      }
    }
    it->second->ProcessUdpPacket(packet_info.self_address,
                                 packet_info.peer_address, packet_info.packet);
    return true;
  }
  if (packet_info.version.IsKnown() &&
      !GetQuicRestartFlag(quic_map_original_connection_ids2)) {
    // We did not find the connection ID, check if we've replaced it.
    // This is only performed for supported versions because packets with
    // unsupported versions can flow through this function in order to send
    // a version negotiation packet, but we know that their connection ID
    // did not get replaced since that is performed on connection creation,
    // and that only happens for known verions.
    // There is no need to perform this check if
    // |reference_counted_session_map_| is storing original connection IDs
    // separately. It can be counterproductive to do this check if that
    // consumes a nonce or generates a random connection ID.
    absl::optional<QuicConnectionId> replaced_connection_id =
        MaybeReplaceServerConnectionId(server_connection_id,
                                       packet_info.version);
    if (replaced_connection_id.has_value()) {
      // Search for the replacement.
      auto it2 = reference_counted_session_map_.find(*replaced_connection_id);
      if (it2 != reference_counted_session_map_.end()) {
        QUICHE_DCHECK(
            !buffered_packets_.HasBufferedPackets(*replaced_connection_id));
        it2->second->ProcessUdpPacket(packet_info.self_address,
                                      packet_info.peer_address,
                                      packet_info.packet);
        return true;
      }
    }
  }

  if (buffered_packets_.HasChloForConnection(server_connection_id)) {
    BufferEarlyPacket(packet_info);
    return true;
  }

  if (OnFailedToDispatchPacket(packet_info)) {
    return true;
  }

  if (time_wait_list_manager_->IsConnectionIdInTimeWait(server_connection_id)) {
    // This connection ID is already in time-wait state.
    time_wait_list_manager_->ProcessPacket(
        packet_info.self_address, packet_info.peer_address,
        packet_info.destination_connection_id, packet_info.form,
        packet_info.packet.length(), GetPerPacketContext());
    return true;
  }

  // The packet has an unknown connection ID.
  if (!accept_new_connections_ && packet_info.version_flag) {
    // If not accepting new connections, reject packets with version which can
    // potentially result in new connection creation. But if the packet doesn't
    // have version flag, leave it to ValidityChecks() to reset it.
    // By adding the connection to time wait list, following packets on this
    // connection will not reach ShouldAcceptNewConnections().
    StatelesslyTerminateConnection(
        packet_info.destination_connection_id, packet_info.form,
        packet_info.version_flag, packet_info.use_length_prefix,
        packet_info.version, QUIC_HANDSHAKE_FAILED,
        "Stop accepting new connections",
        quic::QuicTimeWaitListManager::SEND_STATELESS_RESET);
    // Time wait list will reject the packet correspondingly..
    time_wait_list_manager()->ProcessPacket(
        packet_info.self_address, packet_info.peer_address,
        packet_info.destination_connection_id, packet_info.form,
        packet_info.packet.length(), GetPerPacketContext());
    OnNewConnectionRejected();
    return true;
  }

  // Unless the packet provides a version, assume that we can continue
  // processing using our preferred version.
  if (packet_info.version_flag) {
    if (!IsSupportedVersion(packet_info.version)) {
      if (ShouldCreateSessionForUnknownVersion(packet_info.version_label)) {
        return false;
      }
      if (!crypto_config()->validate_chlo_size() ||
          packet_info.packet.length() >= kMinPacketSizeForVersionNegotiation) {
        // Since the version is not supported, send a version negotiation
        // packet and stop processing the current packet.
        QuicConnectionId client_connection_id =
            packet_info.source_connection_id;
        time_wait_list_manager()->SendVersionNegotiationPacket(
            server_connection_id, client_connection_id,
            packet_info.form != GOOGLE_QUIC_PACKET,
            packet_info.use_length_prefix, GetSupportedVersions(),
            packet_info.self_address, packet_info.peer_address,
            GetPerPacketContext());
      }
      return true;
    }

    if (crypto_config()->validate_chlo_size() &&
        packet_info.form == IETF_QUIC_LONG_HEADER_PACKET &&
        packet_info.long_packet_type == INITIAL &&
        packet_info.packet.length() < kMinClientInitialPacketLength) {
      QUIC_DVLOG(1) << "Dropping initial packet which is too short, length: "
                    << packet_info.packet.length();
      QUIC_CODE_COUNT(quic_drop_small_initial_packets);
      return true;
    }
  }

  return false;
}

void QuicDispatcher::ProcessHeader(ReceivedPacketInfo* packet_info) {
  QuicConnectionId server_connection_id =
      packet_info->destination_connection_id;
  // Packet's connection ID is unknown.  Apply the validity checks.
  QuicPacketFate fate = ValidityChecks(*packet_info);

  // |connection_close_error_code| is used if the final packet fate is
  // kFateTimeWait.
  QuicErrorCode connection_close_error_code = QUIC_HANDSHAKE_FAILED;

  // If a fatal TLS alert was received when extracting Client Hello,
  // |tls_alert_error_detail| will be set and will be used as the error_details
  // of the connection close.
  std::string tls_alert_error_detail;

  if (fate == kFateProcess) {
    ExtractChloResult extract_chlo_result =
        TryExtractChloOrBufferEarlyPacket(*packet_info);
    auto& parsed_chlo = extract_chlo_result.parsed_chlo;

    if (send_connection_close_for_tls_alerts_ &&
        extract_chlo_result.tls_alert.has_value()) {
      QUIC_RESTART_FLAG_COUNT_N(
          quic_dispatcher_send_connection_close_for_tls_alerts, 2, 3);
      QUIC_BUG_IF(quic_dispatcher_parsed_chlo_and_tls_alert_coexist_1,
                  parsed_chlo.has_value())
          << "parsed_chlo and tls_alert should not be set at the same time.";
      // Fatal TLS alert when parsing Client Hello.
      fate = kFateTimeWait;
      uint8_t tls_alert = *extract_chlo_result.tls_alert;
      connection_close_error_code = TlsAlertToQuicErrorCode(tls_alert);
      tls_alert_error_detail =
          absl::StrCat("TLS handshake failure (",
                       EncryptionLevelToString(ENCRYPTION_INITIAL), ") ",
                       static_cast<int>(tls_alert), ": ",
                       SSL_alert_desc_string_long(tls_alert));
    } else if (!parsed_chlo.has_value()) {
      // Client Hello incomplete. Packet has been buffered or (rarely) dropped.
      return;
    } else {
      // Client Hello fully received.
      fate = ValidityChecksOnFullChlo(*packet_info, *parsed_chlo);

      if (fate == kFateProcess) {
        QUICHE_DCHECK(
            parsed_chlo->legacy_version_encapsulation_inner_packet.empty() ||
            !packet_info->version.UsesTls());
        if (GetQuicRestartFlag(quic_disable_legacy_version_encapsulation)) {
          if (!parsed_chlo->legacy_version_encapsulation_inner_packet.empty()) {
            QUIC_CODE_COUNT(
                quic_disable_legacy_version_encapsulation_process_header);
          }
        } else {
          if (MaybeHandleLegacyVersionEncapsulation(
                  this, parsed_chlo->legacy_version_encapsulation_inner_packet,
                  *packet_info)) {
            return;
          }
        }

        ProcessChlo(*std::move(parsed_chlo), packet_info);
        return;
      }
    }
  }

  switch (fate) {
    case kFateProcess:
      // kFateProcess have been processed above.
      QUIC_BUG(quic_dispatcher_bad_packet_fate) << fate;
      break;
    case kFateTimeWait: {
      // Add this connection_id to the time-wait state, to safely reject
      // future packets.
      QUIC_DLOG(INFO) << "Adding connection ID " << server_connection_id
                      << " to time-wait list.";
      QUIC_CODE_COUNT(quic_reject_fate_time_wait);
      const std::string& connection_close_error_detail =
          tls_alert_error_detail.empty() ? "Reject connection"
                                         : tls_alert_error_detail;
      StatelesslyTerminateConnection(
          server_connection_id, packet_info->form, packet_info->version_flag,
          packet_info->use_length_prefix, packet_info->version,
          connection_close_error_code, connection_close_error_detail,
          quic::QuicTimeWaitListManager::SEND_STATELESS_RESET);

      QUICHE_DCHECK(time_wait_list_manager_->IsConnectionIdInTimeWait(
          server_connection_id));
      time_wait_list_manager_->ProcessPacket(
          packet_info->self_address, packet_info->peer_address,
          server_connection_id, packet_info->form, packet_info->packet.length(),
          GetPerPacketContext());

      buffered_packets_.DiscardPackets(server_connection_id);
    } break;
    case kFateDrop:
      break;
  }
}

QuicDispatcher::ExtractChloResult
QuicDispatcher::TryExtractChloOrBufferEarlyPacket(
    const ReceivedPacketInfo& packet_info) {
  ExtractChloResult result;
  if (packet_info.version.UsesTls()) {
    bool has_full_tls_chlo = false;
    std::string sni;
    std::vector<std::string> alpns;
    bool resumption_attempted = false, early_data_attempted = false;
    if (buffered_packets_.HasBufferedPackets(
            packet_info.destination_connection_id)) {
      // If we already have buffered packets for this connection ID,
      // use the associated TlsChloExtractor to parse this packet.
      has_full_tls_chlo = buffered_packets_.IngestPacketForTlsChloExtraction(
          packet_info.destination_connection_id, packet_info.version,
          packet_info.packet, &alpns, &sni, &resumption_attempted,
          &early_data_attempted, &result.tls_alert);
    } else {
      // If we do not have a BufferedPacketList for this connection ID,
      // create a single-use one to check whether this packet contains a
      // full single-packet CHLO.
      TlsChloExtractor tls_chlo_extractor;
      tls_chlo_extractor.IngestPacket(packet_info.version, packet_info.packet);
      if (tls_chlo_extractor.HasParsedFullChlo()) {
        // This packet contains a full single-packet CHLO.
        has_full_tls_chlo = true;
        alpns = tls_chlo_extractor.alpns();
        sni = tls_chlo_extractor.server_name();
        resumption_attempted = tls_chlo_extractor.resumption_attempted();
        early_data_attempted = tls_chlo_extractor.early_data_attempted();
      } else {
        result.tls_alert = tls_chlo_extractor.tls_alert();
      }
    }

    if (send_connection_close_for_tls_alerts_ && result.tls_alert.has_value()) {
      QUIC_RESTART_FLAG_COUNT_N(
          quic_dispatcher_send_connection_close_for_tls_alerts, 3, 3);
      QUIC_BUG_IF(quic_dispatcher_parsed_chlo_and_tls_alert_coexist_2,
                  has_full_tls_chlo)
          << "parsed_chlo and tls_alert should not be set at the same time.";
      return result;
    }

    if (!has_full_tls_chlo) {
      // This packet does not contain a full CHLO. It could be a 0-RTT
      // packet that arrived before the CHLO (due to loss or reordering),
      // or it could be a fragment of a multi-packet CHLO.
      BufferEarlyPacket(packet_info);
      return result;
    }

    ParsedClientHello& parsed_chlo = result.parsed_chlo.emplace();
    parsed_chlo.sni = std::move(sni);
    parsed_chlo.alpns = std::move(alpns);
    if (packet_info.retry_token.has_value()) {
      parsed_chlo.retry_token = std::string(*packet_info.retry_token);
    }
    parsed_chlo.resumption_attempted = resumption_attempted;
    parsed_chlo.early_data_attempted = early_data_attempted;
    return result;
  }

  ChloAlpnSniExtractor alpn_extractor;
  if (GetQuicFlag(FLAGS_quic_allow_chlo_buffering) &&
      !ChloExtractor::Extract(packet_info.packet, packet_info.version,
                              config_->create_session_tag_indicators(),
                              &alpn_extractor,
                              packet_info.destination_connection_id.length())) {
    // Buffer non-CHLO packets.
    BufferEarlyPacket(packet_info);
    return result;
  }

  // We only apply this check for versions that do not use the IETF
  // invariant header because those versions are already checked in
  // QuicDispatcher::MaybeDispatchPacket.
  if (packet_info.version_flag &&
      !packet_info.version.HasIetfInvariantHeader() &&
      crypto_config()->validate_chlo_size() &&
      packet_info.packet.length() < kMinClientInitialPacketLength) {
    QUIC_DVLOG(1) << "Dropping CHLO packet which is too short, length: "
                  << packet_info.packet.length();
    QUIC_CODE_COUNT(quic_drop_small_chlo_packets);
    return result;
  }

  ParsedClientHello& parsed_chlo = result.parsed_chlo.emplace();
  parsed_chlo.legacy_version_encapsulation_inner_packet =
      alpn_extractor.ConsumeLegacyVersionEncapsulationInnerPacket();
  parsed_chlo.sni = alpn_extractor.ConsumeSni();
  parsed_chlo.uaid = alpn_extractor.ConsumeUaid();
  parsed_chlo.alpns = {alpn_extractor.ConsumeAlpn()};
  return result;
}

std::string QuicDispatcher::SelectAlpn(const std::vector<std::string>& alpns) {
  if (alpns.empty()) {
    return "";
  }
  if (alpns.size() > 1u) {
    const std::vector<std::string>& supported_alpns =
        version_manager_->GetSupportedAlpns();
    for (const std::string& alpn : alpns) {
      if (std::find(supported_alpns.begin(), supported_alpns.end(), alpn) !=
          supported_alpns.end()) {
        return alpn;
      }
    }
  }
  return alpns[0];
}

QuicDispatcher::QuicPacketFate QuicDispatcher::ValidityChecks(
    const ReceivedPacketInfo& packet_info) {
  if (!packet_info.version_flag) {
    QUIC_DLOG(INFO)
        << "Packet without version arrived for unknown connection ID "
        << packet_info.destination_connection_id;
    MaybeResetPacketsWithNoVersion(packet_info);
    return kFateDrop;
  }

  // Let the connection parse and validate packet number.
  return kFateProcess;
}

void QuicDispatcher::CleanUpSession(QuicConnectionId server_connection_id,
                                    QuicConnection* connection,
                                    QuicErrorCode /*error*/,
                                    const std::string& /*error_details*/,
                                    ConnectionCloseSource /*source*/) {
  write_blocked_list_.erase(connection);
  QuicTimeWaitListManager::TimeWaitAction action =
      QuicTimeWaitListManager::SEND_STATELESS_RESET;
  if (connection->termination_packets() != nullptr &&
      !connection->termination_packets()->empty()) {
    action = QuicTimeWaitListManager::SEND_CONNECTION_CLOSE_PACKETS;
  } else {
    if (!connection->IsHandshakeComplete()) {
      // TODO(fayang): Do not serialize connection close packet if the
      // connection is closed by the client.
      if (!connection->version().HasIetfInvariantHeader()) {
        QUIC_CODE_COUNT(gquic_add_to_time_wait_list_with_handshake_failed);
      } else {
        QUIC_CODE_COUNT(quic_v44_add_to_time_wait_list_with_handshake_failed);
      }
      // This serializes a connection close termination packet and adds the
      // connection to the time wait list.
      StatelessConnectionTerminator terminator(
          server_connection_id,
          connection->GetOriginalDestinationConnectionId(),
          connection->version(), helper_.get(), time_wait_list_manager_.get());
      terminator.CloseConnection(
          QUIC_HANDSHAKE_FAILED,
          "Connection is closed by server before handshake confirmed",
          connection->version().HasIetfInvariantHeader(),
          connection->GetActiveServerConnectionIds());
      return;
    }
    QUIC_CODE_COUNT(quic_v44_add_to_time_wait_list_with_stateless_reset);
  }
  time_wait_list_manager_->AddConnectionIdToTimeWait(
      action,
      TimeWaitConnectionInfo(
          connection->version().HasIetfInvariantHeader(),
          connection->termination_packets(),
          connection->GetActiveServerConnectionIds(),
          connection->sent_packet_manager().GetRttStats()->smoothed_rtt()));
}

void QuicDispatcher::StartAcceptingNewConnections() {
  accept_new_connections_ = true;
}

void QuicDispatcher::StopAcceptingNewConnections() {
  accept_new_connections_ = false;
  // No more CHLO will arrive and buffered CHLOs shouldn't be able to create
  // connections.
  buffered_packets_.DiscardAllPackets();
}

void QuicDispatcher::PerformActionOnActiveSessions(
    std::function<void(QuicSession*)> operation) const {
  absl::flat_hash_set<QuicSession*> visited_session;
  visited_session.reserve(reference_counted_session_map_.size());
  for (auto const& kv : reference_counted_session_map_) {
    QuicSession* session = kv.second.get();
    if (visited_session.insert(session).second) {
      operation(session);
    }
  }
}

// Get a snapshot of all sessions.
std::vector<std::shared_ptr<QuicSession>> QuicDispatcher::GetSessionsSnapshot()
    const {
  std::vector<std::shared_ptr<QuicSession>> snapshot;
  snapshot.reserve(reference_counted_session_map_.size());
  absl::flat_hash_set<QuicSession*> visited_session;
  visited_session.reserve(reference_counted_session_map_.size());
  for (auto const& kv : reference_counted_session_map_) {
    QuicSession* session = kv.second.get();
    if (visited_session.insert(session).second) {
      snapshot.push_back(kv.second);
    }
  }
  return snapshot;
}

std::unique_ptr<QuicPerPacketContext> QuicDispatcher::GetPerPacketContext()
    const {
  return nullptr;
}

void QuicDispatcher::DeleteSessions() {
  if (!write_blocked_list_.empty()) {
    for (const auto& session : closed_session_list_) {
      if (write_blocked_list_.erase(session->connection()) != 0) {
        QUIC_BUG(quic_bug_12724_2)
            << "QuicConnection was in WriteBlockedList before destruction "
            << session->connection()->connection_id();
      }
    }
  }
  closed_session_list_.clear();
}

void QuicDispatcher::ClearStatelessResetAddresses() {
  recent_stateless_reset_addresses_.clear();
}

void QuicDispatcher::OnCanWrite() {
  // The socket is now writable.
  writer_->SetWritable();

  // Move every blocked writer in |write_blocked_list_| to a temporary list.
  const size_t num_blocked_writers_before = write_blocked_list_.size();
  WriteBlockedList temp_list;
  temp_list.swap(write_blocked_list_);
  QUICHE_DCHECK(write_blocked_list_.empty());

  // Give each blocked writer a chance to write what they indended to write.
  // If they are blocked again, they will call |OnWriteBlocked| to add
  // themselves back into |write_blocked_list_|.
  while (!temp_list.empty()) {
    QuicBlockedWriterInterface* blocked_writer = temp_list.begin()->first;
    temp_list.erase(temp_list.begin());
    blocked_writer->OnBlockedWriterCanWrite();
  }
  const size_t num_blocked_writers_after = write_blocked_list_.size();
  if (num_blocked_writers_after != 0) {
    if (num_blocked_writers_before == num_blocked_writers_after) {
      QUIC_CODE_COUNT(quic_zero_progress_on_can_write);
    } else {
      QUIC_CODE_COUNT(quic_blocked_again_on_can_write);
    }
  }
}

bool QuicDispatcher::HasPendingWrites() const {
  return !write_blocked_list_.empty();
}

void QuicDispatcher::Shutdown() {
  while (!reference_counted_session_map_.empty()) {
    QuicSession* session = reference_counted_session_map_.begin()->second.get();
    session->connection()->CloseConnection(
        QUIC_PEER_GOING_AWAY, "Server shutdown imminent",
        ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
    // Validate that the session removes itself from the session map on close.
    QUICHE_DCHECK(reference_counted_session_map_.empty() ||
                  reference_counted_session_map_.begin()->second.get() !=
                      session);
  }
  DeleteSessions();
}

void QuicDispatcher::OnConnectionClosed(QuicConnectionId server_connection_id,
                                        QuicErrorCode error,
                                        const std::string& error_details,
                                        ConnectionCloseSource source) {
  auto it = reference_counted_session_map_.find(server_connection_id);
  if (it == reference_counted_session_map_.end()) {
    QUIC_BUG(quic_bug_10287_3) << "ConnectionId " << server_connection_id
                               << " does not exist in the session map.  Error: "
                               << QuicErrorCodeToString(error);
    QUIC_BUG(quic_bug_10287_4) << QuicStackTrace();
    return;
  }

  QUIC_DLOG_IF(INFO, error != QUIC_NO_ERROR)
      << "Closing connection (" << server_connection_id
      << ") due to error: " << QuicErrorCodeToString(error)
      << ", with details: " << error_details;

  QuicConnection* connection = it->second->connection();
  // Set up alarm to fire immediately to bring destruction of this session
  // out of current call stack.
  if (closed_session_list_.empty()) {
    delete_sessions_alarm_->Update(helper()->GetClock()->ApproximateNow(),
                                   QuicTime::Delta::Zero());
  }
  closed_session_list_.push_back(std::move(it->second));
  CleanUpSession(it->first, connection, error, error_details, source);
  for (const QuicConnectionId& cid :
       connection->GetActiveServerConnectionIds()) {
    reference_counted_session_map_.erase(cid);
  }
  --num_sessions_in_session_map_;
}

void QuicDispatcher::OnWriteBlocked(
    QuicBlockedWriterInterface* blocked_writer) {
  if (!blocked_writer->IsWriterBlocked()) {
    // It is a programming error if this ever happens. When we are sure it is
    // not happening, replace it with a QUICHE_DCHECK.
    QUIC_BUG(quic_bug_12724_4)
        << "Tried to add writer into blocked list when it shouldn't be added";
    // Return without adding the connection to the blocked list, to avoid
    // infinite loops in OnCanWrite.
    return;
  }

  write_blocked_list_.insert(std::make_pair(blocked_writer, true));
}

void QuicDispatcher::OnRstStreamReceived(const QuicRstStreamFrame& /*frame*/) {}

void QuicDispatcher::OnStopSendingReceived(
    const QuicStopSendingFrame& /*frame*/) {}

bool QuicDispatcher::TryAddNewConnectionId(
    const QuicConnectionId& server_connection_id,
    const QuicConnectionId& new_connection_id) {
  auto it = reference_counted_session_map_.find(server_connection_id);
  if (it == reference_counted_session_map_.end()) {
    QUIC_BUG(quic_bug_10287_7)
        << "Couldn't locate the session that issues the connection ID in "
           "reference_counted_session_map_.  server_connection_id:"
        << server_connection_id << " new_connection_id: " << new_connection_id;
    return false;
  }
  // Count new connection ID added to the dispatcher map.
  QUIC_RELOADABLE_FLAG_COUNT_N(quic_connection_migration_use_new_cid_v2, 6, 6);
  auto insertion_result = reference_counted_session_map_.insert(
      std::make_pair(new_connection_id, it->second));
  if (!insertion_result.second) {
    QUIC_CODE_COUNT(quic_cid_already_in_session_map);
  }
  return insertion_result.second;
}

void QuicDispatcher::OnConnectionIdRetired(
    const QuicConnectionId& server_connection_id) {
  reference_counted_session_map_.erase(server_connection_id);
}

void QuicDispatcher::OnConnectionAddedToTimeWaitList(
    QuicConnectionId server_connection_id) {
  QUIC_DLOG(INFO) << "Connection " << server_connection_id
                  << " added to time wait list.";
}

void QuicDispatcher::StatelesslyTerminateConnection(
    QuicConnectionId server_connection_id, PacketHeaderFormat format,
    bool version_flag, bool use_length_prefix, ParsedQuicVersion version,
    QuicErrorCode error_code, const std::string& error_details,
    QuicTimeWaitListManager::TimeWaitAction action) {
  if (format != IETF_QUIC_LONG_HEADER_PACKET && !version_flag) {
    QUIC_DVLOG(1) << "Statelessly terminating " << server_connection_id
                  << " based on a non-ietf-long packet, action:" << action
                  << ", error_code:" << error_code
                  << ", error_details:" << error_details;
    time_wait_list_manager_->AddConnectionIdToTimeWait(
        action, TimeWaitConnectionInfo(format != GOOGLE_QUIC_PACKET, nullptr,
                                       {server_connection_id}));
    return;
  }

  // If the version is known and supported by framer, send a connection close.
  if (IsSupportedVersion(version)) {
    QUIC_DVLOG(1)
        << "Statelessly terminating " << server_connection_id
        << " based on an ietf-long packet, which has a supported version:"
        << version << ", error_code:" << error_code
        << ", error_details:" << error_details;

    StatelessConnectionTerminator terminator(
        server_connection_id, server_connection_id, version, helper_.get(),
        time_wait_list_manager_.get());
    // This also adds the connection to time wait list.
    terminator.CloseConnection(
        error_code, error_details, format != GOOGLE_QUIC_PACKET,
        /*active_connection_ids=*/{server_connection_id});
    QUIC_CODE_COUNT(quic_dispatcher_generated_connection_close);
    QuicSession::RecordConnectionCloseAtServer(
        error_code, ConnectionCloseSource::FROM_SELF);
    return;
  }

  QUIC_DVLOG(1)
      << "Statelessly terminating " << server_connection_id
      << " based on an ietf-long packet, which has an unsupported version:"
      << version << ", error_code:" << error_code
      << ", error_details:" << error_details;
  // Version is unknown or unsupported by framer, send a version negotiation
  // with an empty version list, which can be understood by the client.
  std::vector<std::unique_ptr<QuicEncryptedPacket>> termination_packets;
  termination_packets.push_back(QuicFramer::BuildVersionNegotiationPacket(
      server_connection_id, EmptyQuicConnectionId(),
      /*ietf_quic=*/format != GOOGLE_QUIC_PACKET, use_length_prefix,
      /*versions=*/{}));
  time_wait_list_manager()->AddConnectionIdToTimeWait(
      QuicTimeWaitListManager::SEND_TERMINATION_PACKETS,
      TimeWaitConnectionInfo(/*ietf_quic=*/format != GOOGLE_QUIC_PACKET,
                             &termination_packets, {server_connection_id}));
}

bool QuicDispatcher::ShouldCreateSessionForUnknownVersion(
    QuicVersionLabel /*version_label*/) {
  return false;
}

void QuicDispatcher::OnExpiredPackets(
    QuicConnectionId server_connection_id,
    BufferedPacketList early_arrived_packets) {
  QUIC_CODE_COUNT(quic_reject_buffered_packets_expired);
  StatelesslyTerminateConnection(
      server_connection_id,
      early_arrived_packets.ietf_quic ? IETF_QUIC_LONG_HEADER_PACKET
                                      : GOOGLE_QUIC_PACKET,
      /*version_flag=*/true,
      early_arrived_packets.version.HasLengthPrefixedConnectionIds(),
      early_arrived_packets.version, QUIC_HANDSHAKE_FAILED,
      "Packets buffered for too long",
      quic::QuicTimeWaitListManager::SEND_STATELESS_RESET);
}

void QuicDispatcher::ProcessBufferedChlos(size_t max_connections_to_create) {
  // Reset the counter before starting creating connections.
  new_sessions_allowed_per_event_loop_ = max_connections_to_create;
  for (; new_sessions_allowed_per_event_loop_ > 0;
       --new_sessions_allowed_per_event_loop_) {
    QuicConnectionId server_connection_id;
    BufferedPacketList packet_list =
        buffered_packets_.DeliverPacketsForNextConnection(
            &server_connection_id);
    const std::list<BufferedPacket>& packets = packet_list.buffered_packets;
    if (packets.empty()) {
      return;
    }
    if (!packet_list.parsed_chlo.has_value()) {
      QUIC_BUG(quic_dispatcher_no_parsed_chlo_in_buffered_packets)
          << "Buffered connection has no CHLO. connection_id:"
          << server_connection_id;
      continue;
    }
    auto session_ptr = QuicDispatcher::CreateSessionFromChlo(
        server_connection_id, *packet_list.parsed_chlo, packet_list.version,
        packets.front().self_address, packets.front().peer_address);
    if (session_ptr != nullptr) {
      DeliverPacketsToSession(packets, session_ptr.get());
      if (server_connection_id != session_ptr->connection_id() &&
          GetQuicRestartFlag(quic_map_original_connection_ids2)) {
        QUIC_RESTART_FLAG_COUNT_N(quic_map_original_connection_ids2, 1, 4);
      }
    }
  }
}

bool QuicDispatcher::HasChlosBuffered() const {
  return buffered_packets_.HasChlosBuffered();
}

// Return true if there is any packet buffered in the store.
bool QuicDispatcher::HasBufferedPackets(QuicConnectionId server_connection_id) {
  return buffered_packets_.HasBufferedPackets(server_connection_id);
}

void QuicDispatcher::OnBufferPacketFailure(
    EnqueuePacketResult result, QuicConnectionId server_connection_id) {
  QUIC_DLOG(INFO) << "Fail to buffer packet on connection "
                  << server_connection_id << " because of " << result;
}

QuicTimeWaitListManager* QuicDispatcher::CreateQuicTimeWaitListManager() {
  return new QuicTimeWaitListManager(writer_.get(), this, helper_->GetClock(),
                                     alarm_factory_.get());
}

void QuicDispatcher::BufferEarlyPacket(const ReceivedPacketInfo& packet_info) {
  EnqueuePacketResult rs = buffered_packets_.EnqueuePacket(
      packet_info.destination_connection_id,
      packet_info.form != GOOGLE_QUIC_PACKET, packet_info.packet,
      packet_info.self_address, packet_info.peer_address, packet_info.version,
      /*parsed_chlo=*/absl::nullopt);
  if (rs != EnqueuePacketResult::SUCCESS) {
    OnBufferPacketFailure(rs, packet_info.destination_connection_id);
  }
}

void QuicDispatcher::ProcessChlo(ParsedClientHello parsed_chlo,
                                 ReceivedPacketInfo* packet_info) {
  if (GetQuicFlag(FLAGS_quic_allow_chlo_buffering) &&
      new_sessions_allowed_per_event_loop_ <= 0) {
    // Can't create new session any more. Wait till next event loop.
    QUIC_BUG_IF(quic_bug_12724_7, buffered_packets_.HasChloForConnection(
                                      packet_info->destination_connection_id));
    EnqueuePacketResult rs = buffered_packets_.EnqueuePacket(
        packet_info->destination_connection_id,
        packet_info->form != GOOGLE_QUIC_PACKET, packet_info->packet,
        packet_info->self_address, packet_info->peer_address,
        packet_info->version, std::move(parsed_chlo));
    if (rs != EnqueuePacketResult::SUCCESS) {
      OnBufferPacketFailure(rs, packet_info->destination_connection_id);
    }
    return;
  }

  auto session_ptr = QuicDispatcher::CreateSessionFromChlo(
      packet_info->destination_connection_id, parsed_chlo, packet_info->version,
      packet_info->self_address, packet_info->peer_address);
  if (session_ptr == nullptr) {
    return;
  }
  std::list<BufferedPacket> packets =
      buffered_packets_.DeliverPackets(packet_info->destination_connection_id)
          .buffered_packets;
  if (packet_info->destination_connection_id != session_ptr->connection_id()) {
    // Provide the calling function with access to the new connection ID.
    packet_info->destination_connection_id = session_ptr->connection_id();
    if (!packets.empty()) {
      QUIC_CODE_COUNT(
          quic_delivered_buffered_packets_to_connection_with_replaced_id);
    }
    if (GetQuicRestartFlag(quic_map_original_connection_ids2)) {
      QUIC_RESTART_FLAG_COUNT_N(quic_map_original_connection_ids2, 2, 4);
    }
  }
  // Process CHLO at first.
  session_ptr->ProcessUdpPacket(packet_info->self_address,
                                packet_info->peer_address, packet_info->packet);
  // Deliver queued-up packets in the same order as they arrived.
  // Do this even when flag is off because there might be still some packets
  // buffered in the store before flag is turned off.
  DeliverPacketsToSession(packets, session_ptr.get());
  --new_sessions_allowed_per_event_loop_;
}

void QuicDispatcher::SetLastError(QuicErrorCode error) { last_error_ = error; }

bool QuicDispatcher::OnFailedToDispatchPacket(
    const ReceivedPacketInfo& /*packet_info*/) {
  return false;
}

const ParsedQuicVersionVector& QuicDispatcher::GetSupportedVersions() {
  return version_manager_->GetSupportedVersions();
}

void QuicDispatcher::DeliverPacketsToSession(
    const std::list<BufferedPacket>& packets, QuicSession* session) {
  for (const BufferedPacket& packet : packets) {
    session->ProcessUdpPacket(packet.self_address, packet.peer_address,
                              *(packet.packet));
  }
}

bool QuicDispatcher::IsSupportedVersion(const ParsedQuicVersion version) {
  for (const ParsedQuicVersion& supported_version :
       version_manager_->GetSupportedVersions()) {
    if (version == supported_version) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<QuicSession> QuicDispatcher::CreateSessionFromChlo(
    const QuicConnectionId original_connection_id,
    const ParsedClientHello& parsed_chlo, const ParsedQuicVersion version,
    const QuicSocketAddress self_address,
    const QuicSocketAddress peer_address) {
  absl::optional<QuicConnectionId> server_connection_id =
      MaybeReplaceServerConnectionId(original_connection_id, version);
  const bool replaced_connection_id = server_connection_id.has_value();
  if (!replaced_connection_id) {
    server_connection_id = original_connection_id;
  }
  if (reference_counted_session_map_.count(*server_connection_id) > 0 &&
      GetQuicRestartFlag(quic_map_original_connection_ids2)) {
    // The new connection ID is owned by another session. Avoid creating one
    // altogether, as this connection attempt cannot possibly succeed.
    if (replaced_connection_id) {
      // The original connection ID does not correspond to an existing
      // session. It is safe to send CONNECTION_CLOSE and add to TIME_WAIT.
      StatelesslyTerminateConnection(
          original_connection_id, IETF_QUIC_LONG_HEADER_PACKET,
          /*version_flag=*/true, version.HasLengthPrefixedConnectionIds(),
          version, QUIC_HANDSHAKE_FAILED,
          "Connection ID collision, please retry",
          QuicTimeWaitListManager::SEND_CONNECTION_CLOSE_PACKETS);
    }
    return nullptr;
  }
  // Creates a new session and process all buffered packets for this connection.
  std::string alpn = SelectAlpn(parsed_chlo.alpns);
  std::unique_ptr<QuicSession> session =
      CreateQuicSession(*server_connection_id, self_address, peer_address, alpn,
                        version, parsed_chlo);
  if (ABSL_PREDICT_FALSE(session == nullptr)) {
    QUIC_BUG(quic_bug_10287_8)
        << "CreateQuicSession returned nullptr for " << *server_connection_id
        << " from " << peer_address << " to " << self_address << " ALPN \""
        << alpn << "\" version " << version;
    return nullptr;
  }

  if (replaced_connection_id) {
    session->connection()->SetOriginalDestinationConnectionId(
        original_connection_id);
  }
  QUIC_DLOG(INFO) << "Created new session for " << *server_connection_id;

  auto insertion_result = reference_counted_session_map_.insert(std::make_pair(
      *server_connection_id, std::shared_ptr<QuicSession>(std::move(session))));
  std::shared_ptr<QuicSession> session_ptr = insertion_result.first->second;
  if (!insertion_result.second) {
    QUIC_BUG(quic_bug_10287_9)
        << "Tried to add a session to session_map with existing "
           "connection id: "
        << *server_connection_id;
  } else {
    ++num_sessions_in_session_map_;
    if (GetQuicRestartFlag(quic_map_original_connection_ids2) &&
        replaced_connection_id) {
      auto insertion_result2 = reference_counted_session_map_.insert(
          std::make_pair(original_connection_id, session_ptr));
      QUIC_BUG_IF(quic_460317833_02, !insertion_result2.second)
          << "Original connection ID already in session_map: "
          << original_connection_id;
      // If insertion of the original connection ID fails, it might cause
      // loss of 0-RTT and other first flight packets, but the connection
      // will usually progress.
    }
  }
  return session_ptr;
}

void QuicDispatcher::MaybeResetPacketsWithNoVersion(
    const ReceivedPacketInfo& packet_info) {
  QUICHE_DCHECK(!packet_info.version_flag);
  // Do not send a stateless reset if a reset has been sent to this address
  // recently.
  if (recent_stateless_reset_addresses_.contains(packet_info.peer_address)) {
    QUIC_CODE_COUNT(quic_donot_send_reset_repeatedly);
    return;
  }
  if (packet_info.form != GOOGLE_QUIC_PACKET) {
    // Drop IETF packets smaller than the minimal stateless reset length.
    if (packet_info.packet.length() <=
        QuicFramer::GetMinStatelessResetPacketLength()) {
      QUIC_CODE_COUNT(quic_drop_too_small_short_header_packets);
      return;
    }
  } else {
    const size_t MinValidPacketLength =
        kPacketHeaderTypeSize + expected_server_connection_id_length_ +
        PACKET_1BYTE_PACKET_NUMBER + /*payload size=*/1 + /*tag size=*/12;
    if (packet_info.packet.length() < MinValidPacketLength) {
      // The packet size is too small.
      QUIC_CODE_COUNT(drop_too_small_packets);
      return;
    }
  }
  // Do not send a stateless reset if there are too many stateless reset
  // addresses.
  if (recent_stateless_reset_addresses_.size() >=
      GetQuicFlag(FLAGS_quic_max_recent_stateless_reset_addresses)) {
    QUIC_CODE_COUNT(quic_too_many_recent_reset_addresses);
    return;
  }
  if (recent_stateless_reset_addresses_.empty()) {
    clear_stateless_reset_addresses_alarm_->Update(
        helper()->GetClock()->ApproximateNow() +
            QuicTime::Delta::FromMilliseconds(GetQuicFlag(
                FLAGS_quic_recent_stateless_reset_addresses_lifetime_ms)),
        QuicTime::Delta::Zero());
  }
  recent_stateless_reset_addresses_.emplace(packet_info.peer_address);

  time_wait_list_manager()->SendPublicReset(
      packet_info.self_address, packet_info.peer_address,
      packet_info.destination_connection_id,
      packet_info.form != GOOGLE_QUIC_PACKET, packet_info.packet.length(),
      GetPerPacketContext());
}

size_t QuicDispatcher::NumSessions() const {
  return num_sessions_in_session_map_;
}

}  // namespace quic
