// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/dns/host_resolver_proc.h"

#include <tuple>

#include "build/build_config.h"

#include "base/check.h"
#include "base/threading/scoped_blocking_call.h"
#include "net/base/address_family.h"
#include "net/base/address_list.h"
#include "net/base/net_errors.h"
#include "net/base/sys_addrinfo.h"
#include "net/dns/address_info.h"
#include "net/dns/dns_reloader.h"
#include "net/dns/dns_util.h"
#include "net/dns/host_resolver.h"

#if BUILDFLAG(IS_OPENBSD)
#define AI_ADDRCONFIG 0
#endif

namespace net {

HostResolverProc* HostResolverProc::default_proc_ = nullptr;

HostResolverProc::HostResolverProc(scoped_refptr<HostResolverProc> previous,
                                   bool allow_fallback_to_system_or_default)
    : allow_fallback_to_system_(allow_fallback_to_system_or_default) {
  SetPreviousProc(previous);

  // Implicitly fall-back to the global default procedure.
  if (!previous && allow_fallback_to_system_or_default)
    SetPreviousProc(default_proc_);
}

HostResolverProc::~HostResolverProc() = default;

int HostResolverProc::Resolve(const std::string& host,
                              AddressFamily address_family,
                              HostResolverFlags host_resolver_flags,
                              AddressList* addrlist,
                              int* os_error,
                              handles::NetworkHandle network) {
  if (network == handles::kInvalidNetworkHandle)
    return Resolve(host, address_family, host_resolver_flags, addrlist,
                   os_error);

  NOTIMPLEMENTED();
  return ERR_NOT_IMPLEMENTED;
}

int HostResolverProc::ResolveUsingPrevious(
    const std::string& host,
    AddressFamily address_family,
    HostResolverFlags host_resolver_flags,
    AddressList* addrlist,
    int* os_error) {
  if (previous_proc_.get()) {
    return previous_proc_->Resolve(
        host, address_family, host_resolver_flags, addrlist, os_error);
  }

  // If `allow_fallback_to_system_` is false there is no final fallback. It must
  // be ensured that the Procs can handle any allowed requests. If this check
  // fails while using MockHostResolver or RuleBasedHostResolverProc, it means
  // none of the configured rules matched a host resolution request.
  CHECK(allow_fallback_to_system_);

  // Final fallback is the system resolver.
  return SystemHostResolverCall(host, address_family, host_resolver_flags,
                                addrlist, os_error);
}

void HostResolverProc::SetPreviousProc(scoped_refptr<HostResolverProc> proc) {
  auto current_previous = std::move(previous_proc_);
  // Now that we've guaranteed |this| is the last proc in a chain, we can
  // detect potential cycles using GetLastProc().
  previous_proc_ = (GetLastProc(proc.get()) == this)
                       ? std::move(current_previous)
                       : std::move(proc);
}

void HostResolverProc::SetLastProc(scoped_refptr<HostResolverProc> proc) {
  GetLastProc(this)->SetPreviousProc(std::move(proc));
}

// static
HostResolverProc* HostResolverProc::GetLastProc(HostResolverProc* proc) {
  if (proc == nullptr)
    return nullptr;
  HostResolverProc* last_proc = proc;
  while (last_proc->previous_proc_.get() != nullptr)
    last_proc = last_proc->previous_proc_.get();
  return last_proc;
}

// static
HostResolverProc* HostResolverProc::SetDefault(HostResolverProc* proc) {
  HostResolverProc* old = default_proc_;
  default_proc_ = proc;
  return old;
}

// static
HostResolverProc* HostResolverProc::GetDefault() {
  return default_proc_;
}

namespace {

int AddressFamilyToAF(AddressFamily address_family) {
  switch (address_family) {
    case ADDRESS_FAMILY_IPV4:
      return AF_INET;
    case ADDRESS_FAMILY_IPV6:
      return AF_INET6;
    case ADDRESS_FAMILY_UNSPECIFIED:
      return AF_UNSPEC;
  }
}

}  // namespace

int SystemHostResolverCall(const std::string& host,
                           AddressFamily address_family,
                           HostResolverFlags host_resolver_flags,
                           AddressList* addrlist,
                           int* os_error_opt,
                           handles::NetworkHandle network) {
  // |host| should be a valid domain name. HostResolverImpl::Resolve has checks
  // to fail early if this is not the case.
  DCHECK(IsValidDNSDomain(host));

  struct addrinfo hints = {0};
  hints.ai_family = AddressFamilyToAF(address_family);

#if BUILDFLAG(IS_WIN)
  // DO NOT USE AI_ADDRCONFIG ON WINDOWS.
  //
  // The following comment in <winsock2.h> is the best documentation I found
  // on AI_ADDRCONFIG for Windows:
  //   Flags used in "hints" argument to getaddrinfo()
  //       - AI_ADDRCONFIG is supported starting with Vista
  //       - default is AI_ADDRCONFIG ON whether the flag is set or not
  //         because the performance penalty in not having ADDRCONFIG in
  //         the multi-protocol stack environment is severe;
  //         this defaulting may be disabled by specifying the AI_ALL flag,
  //         in that case AI_ADDRCONFIG must be EXPLICITLY specified to
  //         enable ADDRCONFIG behavior
  //
  // Not only is AI_ADDRCONFIG unnecessary, but it can be harmful.  If the
  // computer is not connected to a network, AI_ADDRCONFIG causes getaddrinfo
  // to fail with WSANO_DATA (11004) for "localhost", probably because of the
  // following note on AI_ADDRCONFIG in the MSDN getaddrinfo page:
  //   The IPv4 or IPv6 loopback address is not considered a valid global
  //   address.
  // See http://crbug.com/5234.
  //
  // OpenBSD does not support it, either.
  hints.ai_flags = 0;
#else
  hints.ai_flags = AI_ADDRCONFIG;
#endif

  // On Linux AI_ADDRCONFIG doesn't consider loopback addresses, even if only
  // loopback addresses are configured. So don't use it when there are only
  // loopback addresses.
  if (host_resolver_flags & HOST_RESOLVER_LOOPBACK_ONLY)
    hints.ai_flags &= ~AI_ADDRCONFIG;

  if (host_resolver_flags & HOST_RESOLVER_CANONNAME)
    hints.ai_flags |= AI_CANONNAME;

#if BUILDFLAG(IS_WIN)
  // See crbug.com/1176970. Flag not documented (other than the declaration
  // comment in ws2def.h) but confirmed by Microsoft to work for this purpose
  // and be safe.
  if (host_resolver_flags & HOST_RESOLVER_AVOID_MULTICAST)
    hints.ai_flags |= AI_DNS_ONLY;
#endif  // BUILDFLAG(IS_WIN)

  // Restrict result set to only this socket type to avoid duplicates.
  hints.ai_socktype = SOCK_STREAM;

  // This function can block for a long time. Use ScopedBlockingCall to increase
  // the current thread pool's capacity and thus avoid reducing CPU usage by the
  // current process during that time.
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::WILL_BLOCK);

#if BUILDFLAG(IS_POSIX) && \
    !(BUILDFLAG(IS_APPLE) || BUILDFLAG(IS_OPENBSD) || BUILDFLAG(IS_ANDROID))
  DnsReloaderMaybeReload();
#endif
  auto [ai, err, os_error] = AddressInfo::Get(host, hints, nullptr, network);
  bool should_retry = false;
  // If the lookup was restricted (either by address family, or address
  // detection), and the results where all localhost of a single family,
  // maybe we should retry.  There were several bugs related to these
  // issues, for example http://crbug.com/42058 and http://crbug.com/49024
  if ((hints.ai_family != AF_UNSPEC || hints.ai_flags & AI_ADDRCONFIG) && ai &&
      ai->IsAllLocalhostOfOneFamily()) {
    if (host_resolver_flags & HOST_RESOLVER_DEFAULT_FAMILY_SET_DUE_TO_NO_IPV6) {
      hints.ai_family = AF_UNSPEC;
      should_retry = true;
    }
    if (hints.ai_flags & AI_ADDRCONFIG) {
      hints.ai_flags &= ~AI_ADDRCONFIG;
      should_retry = true;
    }
  }
  if (should_retry) {
    std::tie(ai, err, os_error) =
        AddressInfo::Get(host, hints, nullptr, network);
  }

  if (os_error_opt)
    *os_error_opt = os_error;

  if (!ai)
    return err;

  *addrlist = ai->CreateAddressList();
  return OK;
}

SystemHostResolverProc::SystemHostResolverProc() : HostResolverProc(nullptr) {}

int SystemHostResolverProc::Resolve(const std::string& hostname,
                                    AddressFamily address_family,
                                    HostResolverFlags host_resolver_flags,
                                    AddressList* addr_list,
                                    int* os_error,
                                    handles::NetworkHandle network) {
  return SystemHostResolverCall(hostname, address_family, host_resolver_flags,
                                addr_list, os_error, network);
}

int SystemHostResolverProc::Resolve(const std::string& hostname,
                                    AddressFamily address_family,
                                    HostResolverFlags host_resolver_flags,
                                    AddressList* addr_list,
                                    int* os_error) {
  return Resolve(hostname, address_family, host_resolver_flags, addr_list,
                 os_error, handles::kInvalidNetworkHandle);
}

SystemHostResolverProc::~SystemHostResolverProc() = default;

ProcTaskParams::ProcTaskParams(scoped_refptr<HostResolverProc> resolver_proc,
                               size_t in_max_retry_attempts)
    : resolver_proc(std::move(resolver_proc)),
      max_retry_attempts(in_max_retry_attempts),
      unresponsive_delay(kDnsDefaultUnresponsiveDelay) {
  // Maximum of 4 retry attempts for host resolution.
  static const size_t kDefaultMaxRetryAttempts = 4u;
  if (max_retry_attempts == HostResolver::ManagerOptions::kDefaultRetryAttempts)
    max_retry_attempts = kDefaultMaxRetryAttempts;
}

ProcTaskParams::ProcTaskParams(const ProcTaskParams& other) = default;

ProcTaskParams::~ProcTaskParams() = default;

}  // namespace net
