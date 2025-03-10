// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_TEST_CERT_BUILDER_H_
#define NET_TEST_CERT_BUILDER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/rand_util.h"
#include "base/strings/string_piece_forward.h"
#include "net/base/ip_address.h"
#include "net/cert/pki/parse_certificate.h"
#include "net/cert/pki/signature_algorithm.h"
#include "net/cert/x509_certificate.h"
#include "third_party/boringssl/src/include/openssl/base.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pool.h"

class GURL;

namespace base {
class FilePath;
}

namespace net {

namespace der {
class Input;
}

// CertBuilder is a helper class to dynamically create a test certificate.
//
// CertBuilder is initialized using an existing certificate, from which it
// copies most properties (see InitFromCert for details).
//
// The subject, serial number, and key for the final certificate are chosen
// randomly. Using a randomized subject and serial number is important to defeat
// certificate caching done by NSS, which otherwise can make test outcomes
// dependent on ordering.
class CertBuilder {
 public:
  // Initializes the CertBuilder, if |orig_cert| is non-null it will be used as
  // a template. If |issuer| is null then the generated certificate will be
  // self-signed. Otherwise, it will be signed using |issuer|.
  CertBuilder(CRYPTO_BUFFER* orig_cert, CertBuilder* issuer);
  ~CertBuilder();

  // Initializes a CertBuilder using the certificate and private key from
  // |cert_and_key_file| as a template. If |issuer| is null then the generated
  // certificate will be self-signed. Otherwise, it will be signed using
  // |issuer|.
  static std::unique_ptr<CertBuilder> FromFile(
      const base::FilePath& cert_and_key_file,
      CertBuilder* issuer);

  // Initializes a CertBuilder that will return a certificate for the provided
  // public key |spki_der|. It will be signed with the |issuer|, this builder
  // will not have a private key, so it cannot produce self-signed certificates
  // and |issuer| cannot be null.
  static std::unique_ptr<CertBuilder> FromSubjectPublicKeyInfo(
      base::span<const uint8_t> spki_der,
      CertBuilder* issuer);

  // Creates a CertBuilder that will return a static |cert| and |key|.
  // This may be passed as the |issuer| param of another CertBuilder to create
  // a cert chain that ends in a pre-defined certificate.
  static std::unique_ptr<CertBuilder> FromStaticCert(CRYPTO_BUFFER* cert,
                                                     EVP_PKEY* key);
  // Like FromStaticCert, but loads the certificate and private key from the
  // PEM file |cert_and_key_file|.
  static std::unique_ptr<CertBuilder> FromStaticCertFile(
      const base::FilePath& cert_and_key_file);

  // Creates a simple leaf->intermediate->root chain of CertBuilders with no AIA
  // or CrlDistributionPoint extensions, and leaf having a subjectAltName of
  // www.example.com.
  static void CreateSimpleChain(std::unique_ptr<CertBuilder>* out_leaf,
                                std::unique_ptr<CertBuilder>* out_intermediate,
                                std::unique_ptr<CertBuilder>* out_root);

  // Creates a simple leaf->root chain of CertBuilders with no AIA or
  // CrlDistributionPoint extensions, and leaf having a subjectAltName of
  // www.example.com.
  static void CreateSimpleChain(std::unique_ptr<CertBuilder>* out_leaf,
                                std::unique_ptr<CertBuilder>* out_root);

  // Returns a compatible signature algorithm for |key|.
  static absl::optional<SignatureAlgorithm> DefaultSignatureAlgorithmForKey(
      EVP_PKEY* key);

  // Signs |tbs_data| with |key| using |signature_algorithm| appending the
  // signature onto |out_signature| and returns true if successful.
  static bool SignData(SignatureAlgorithm signature_algorithm,
                       base::StringPiece tbs_data,
                       EVP_PKEY* key,
                       CBB* out_signature);

  // Returns a DER encoded AlgorithmIdentifier TLV for |signature_algorithm|
  // empty string on error.
  static std::string SignatureAlgorithmToDer(
      SignatureAlgorithm signature_algorithm);

  // Sets a value for the indicated X.509 (v3) extension.
  void SetExtension(const der::Input& oid,
                    std::string value,
                    bool critical = false);

  // Removes an extension (if present).
  void EraseExtension(const der::Input& oid);

  // Sets the basicConstraints extension. |path_len| may be negative to
  // indicate the pathLenConstraint should be omitted.
  void SetBasicConstraints(bool is_ca, int path_len);

  // Sets an AIA extension with a single caIssuers access method.
  void SetCaIssuersUrl(const GURL& url);

  // Sets an AIA extension with the specified caIssuers and OCSP urls. Either
  // list can have 0 or more URLs, but it is an error for both lists to be
  // empty.
  void SetCaIssuersAndOCSPUrls(const std::vector<GURL>& ca_issuers_urls,
                               const std::vector<GURL>& ocsp_urls);

  // Sets a cRLDistributionPoints extension with a single DistributionPoint
  // with |url| in distributionPoint.fullName.
  void SetCrlDistributionPointUrl(const GURL& url);

  // Sets a cRLDistributionPoints extension with a single DistributionPoint
  // with |urls| in distributionPoints.fullName.
  void SetCrlDistributionPointUrls(const std::vector<GURL>& urls);

  // Sets the subject to a Name with a single commonName attribute with
  // the value |common_name| tagged as a UTF8String.
  void SetSubjectCommonName(base::StringPiece common_name);

  // Sets the subject to |subject_tlv|.
  void SetSubject(base::span<const uint8_t> subject_tlv);

  // Sets the SAN for the certificate to a single dNSName.
  void SetSubjectAltName(const std::string& dns_name);

  // Sets the SAN for the certificate to the given dns names and ip addresses.
  void SetSubjectAltNames(const std::vector<std::string>& dns_names,
                          const std::vector<IPAddress>& ip_addresses);

  // Sets the keyUsage extension. |usages| should contain the KeyUsageBit
  // values of the usages to set, and must not be empty.
  void SetKeyUsages(const std::vector<KeyUsageBit>& usages);

  // Sets the extendedKeyUsage extension. |usages| should contain the DER OIDs
  // of the usage purposes to set, and must not be empty.
  void SetExtendedKeyUsages(const std::vector<der::Input>& purpose_oids);

  // Sets the certificatePolicies extension with the specified policyIdentifier
  // OIDs, which must be specified in dotted string notation (e.g. "1.2.3.4").
  void SetCertificatePolicies(const std::vector<std::string>& policy_oids);

  void SetValidity(base::Time not_before, base::Time not_after);

  // Sets the Subject Key Identifier (SKI) extension to the specified string.
  // By default, a unique SKI will be generated for each CertBuilder; however,
  // this may be overridden to force multiple certificates to be considered
  // during path building on systems that prioritize matching SKI to the
  // Authority Key Identifier (AKI) extension, rather than using the
  // Subject/Issuer name. Empty SKIs are not supported; use EraseExtension()
  // for that.
  void SetSubjectKeyIdentifier(const std::string& subject_key_identifier);

  // Sets the Authority Key Identifier (AKI) extension to the specified
  // string.
  // Note: Only the keyIdentifier option is supported, and the value
  // is the raw identifier (i.e. without DER encoding). Empty strings will
  // result in the extension, if present, being erased. This ensures that it
  // is safe to use SetAuthorityKeyIdentifier() with the result of the
  // issuing CertBuilder's (if any) GetSubjectKeyIdentifier() without
  // introducing AKI/SKI chain building issues.
  void SetAuthorityKeyIdentifier(const std::string& authority_key_identifier);

  // Sets the signature algorithm to use in generating the certificate's
  // signature. The signature algorithm should be compatible with
  // the type of |issuer_->GetKey()|. If this method is not called, and the
  // CertBuilder was initialized from a template cert, the signature algorithm
  // of that cert will be used, or if there was no template cert, a default
  // algorithm will be used base on the signing key type.
  void SetSignatureAlgorithm(SignatureAlgorithm signature_algorithm);

  // Sets both signature AlgorithmIdentifier TLVs to encode in the generated
  // certificate.
  // This only affects the bytes written to the output - it does not affect what
  // algorithm is actually used to perform the signature. To set the signature
  // algorithm used to generate the certificate's signature, use
  // |SetSignatureAlgorithm|. If this method is not called, the signature
  // algorithm written to the output will be chosen to match the signature
  // algorithm used to sign the certificate.
  void SetSignatureAlgorithmTLV(base::StringPiece signature_algorithm_tlv);

  // Set only the outer Certificate signatureAlgorithm TLV. See
  // SetSignatureAlgorithmTLV comment for general notes.
  void SetOuterSignatureAlgorithmTLV(base::StringPiece signature_algorithm_tlv);

  // Set only the tbsCertificate signature TLV. See SetSignatureAlgorithmTLV
  // comment for general notes.
  void SetTBSSignatureAlgorithmTLV(base::StringPiece signature_algorithm_tlv);

  void SetRandomSerialNumber();

  // Sets the private key for the generated certificate to an EC key. If a key
  // was already set, it will be replaced.
  void GenerateECKey();

  // Sets the private key for the generated certificate to a 2048-bit RSA key.
  // RSA key generation is expensive, so this should not be used unless an RSA
  // key is specifically needed. If a key was already set, it will be replaced.
  void GenerateRSAKey();

  // Returns the CertBuilder that issues this certificate. (Will be |this| if
  // certificate is self-signed.)
  CertBuilder* issuer() { return issuer_; }

  // Returns a CRYPTO_BUFFER to the generated certificate.
  CRYPTO_BUFFER* GetCertBuffer();

  bssl::UniquePtr<CRYPTO_BUFFER> DupCertBuffer();

  // Returns the subject of the generated certificate.
  const std::string& GetSubject();

  // Returns the serial number for the generated certificate.
  uint64_t GetSerialNumber();

  // Returns the subject key identifier for the generated certificate. If
  // none is present, a random value will be generated.
  // Note: The returned value will be the contents of the OCTET
  // STRING/KeyIdentifier, without DER encoding, ensuring it's suitable for
  // SetSubjectKeyIdentifier().
  std::string GetSubjectKeyIdentifier();

  // Parses and returns validity period for the generated certificate in
  // |not_before| and |not_after|, returning true on success.
  bool GetValidity(base::Time* not_before, base::Time* not_after) const;

  // Returns the key for the generated certificate.
  EVP_PKEY* GetKey();

  // Returns an X509Certificate for the generated certificate.
  scoped_refptr<X509Certificate> GetX509Certificate();

  // Returns an X509Certificate for the generated certificate, including
  // intermediate certificates.
  scoped_refptr<X509Certificate> GetX509CertificateChain();

  // Returns a copy of the certificate's DER.
  std::string GetDER();

 private:
  // Initializes the CertBuilder, if |orig_cert| is non-null it will be used as
  // a template. If |issuer| is null then the generated certificate will be
  // self-signed. Otherwise, it will be signed using |issuer|.
  // |unique_subject_key_identifier| controls whether an ephemeral SKI will
  // be generated for this certificate. In general, any manipulation of the
  // certificate at all should result in a new SKI, to avoid issues on
  // Windows CryptoAPI, but generating a unique SKI can create issues for
  // macOS Security.framework if |orig_cert| has already issued certificates
  // (including self-signed certs). The only time this is safe is thus
  // when used in conjunction with FromStaticCert() and re-using the
  // same key, thus this constructor is private.
  CertBuilder(CRYPTO_BUFFER* orig_cert,
              CertBuilder* issuer,
              bool unique_subject_key_identifier);

  // Marks the generated certificate DER as invalid, so it will need to
  // be re-generated next time the DER is accessed.
  void Invalidate();

  // Generates a random Subject Key Identifier for the certificate. This is
  // necessary for Windows, which otherwises uses SKI/AKI matching for lookups
  // with greater precedence than subject/issuer name matching, and on newer
  // versions of Windows, limits the number of lookups+signature failures that
  // can be performed. Rather than deriving from |key_|, generating a unique
  // value is useful for signalling this is a "unique" and otherwise
  // independent CA.
  void GenerateSubjectKeyIdentifier();

  // Generates a random subject for the certificate, comprised of just a CN.
  void GenerateSubject();

  // Parses |cert| and copies the following properties:
  //   * All extensions (dropping any duplicates)
  //   * Signature algorithm (from Certificate)
  //   * Validity (expiration)
  void InitFromCert(const der::Input& cert);

  // Assembles the CertBuilder into a TBSCertificate.
  void BuildTBSCertificate(base::StringPiece signature_algorithm_tlv,
                           std::string* out);

  void GenerateCertificate();

  struct ExtensionValue {
    bool critical = false;
    std::string value;
  };

  std::string validity_tlv_;
  std::string subject_tlv_;
  absl::optional<SignatureAlgorithm> signature_algorithm_;
  std::string outer_signature_algorithm_tlv_;
  std::string tbs_signature_algorithm_tlv_;
  uint64_t serial_number_ = 0;
  int default_pkey_id_ = EVP_PKEY_EC;

  std::map<std::string, ExtensionValue> extensions_;

  bssl::UniquePtr<CRYPTO_BUFFER> cert_;
  bssl::UniquePtr<EVP_PKEY> key_;

  raw_ptr<CertBuilder> issuer_ = nullptr;
};

}  // namespace net

#endif  // NET_TEST_CERT_BUILDER_H_
