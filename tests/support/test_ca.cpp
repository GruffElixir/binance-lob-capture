#include "support/test_ca.hpp"

#include <memory>
#include <stdexcept>

#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace blc::test {
namespace {
template <class T, void (*Free)(T*)>
using Handle = std::unique_ptr<T, decltype(Free)>;

void require(bool ok) {
    if (!ok) {
        throw std::runtime_error("test certificate generation failed");
    }
}
Handle<EVP_PKEY, EVP_PKEY_free> key() {
    Handle<EVP_PKEY_CTX, EVP_PKEY_CTX_free> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                                                EVP_PKEY_CTX_free);
    require(ctx && EVP_PKEY_keygen_init(ctx.get()) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) == 1);
    EVP_PKEY* out = nullptr;
    require(EVP_PKEY_keygen(ctx.get(), &out) == 1);
    return {out, EVP_PKEY_free};
}
void extension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx{};
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    Handle<X509_EXTENSION, X509_EXTENSION_free> ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value),
                                                    X509_EXTENSION_free);
    require(ext && X509_add_ext(cert, ext.get(), -1) == 1);
}
Handle<X509, X509_free> certificate(EVP_PKEY* public_key, X509* issuer, EVP_PKEY* signing_key,
                                    long serial, bool ca) {
    Handle<X509, X509_free> cert(X509_new(), X509_free);
    require(cert && X509_set_version(cert.get(), 2) == 1);
    require(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400) != nullptr);
    require(X509_set_pubkey(cert.get(), public_key) == 1);
    auto* subject = X509_get_subject_name(cert.get());
    const auto* name = reinterpret_cast<const unsigned char*>(ca ? "BLC test CA" : "loopback");
    require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, name, -1, -1, 0) == 1);
    require(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : subject) ==
            1);
    extension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
              ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
    extension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
              ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature,keyEncipherment");
    if (!ca) {
        extension(cert.get(), issuer, NID_subject_alt_name, "IP:127.0.0.1");
        extension(cert.get(), issuer, NID_ext_key_usage, "serverAuth");
    }
    require(X509_sign(cert.get(), signing_key, EVP_sha256()) > 0);
    return cert;
}
std::string pem(X509* cert, EVP_PKEY* private_part = nullptr) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    require(bio != nullptr);
    require(private_part ? PEM_write_bio_PrivateKey(bio.get(), private_part, nullptr, nullptr, 0,
                                                    nullptr, nullptr) == 1
                         : PEM_write_bio_X509(bio.get(), cert) == 1);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    require(size > 0);
    return {data, static_cast<std::size_t>(size)};
}
}  // namespace

TestCa make_test_ca() {
    auto ca_key = key();
    auto server_key = key();
    auto ca = certificate(ca_key.get(), nullptr, ca_key.get(), 1, true);
    auto server = certificate(server_key.get(), ca.get(), ca_key.get(), 2, false);
    return {pem(ca.get()), pem(server.get()), pem(nullptr, server_key.get())};
}
}  // namespace blc::test
