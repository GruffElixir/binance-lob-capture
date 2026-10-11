#include "blc/net/tls.hpp"

#include "blc/core/errors.hpp"

#include <boost/asio/buffer.hpp>

namespace blc {

namespace ssl = boost::asio::ssl;

ssl::context make_tls_client_context(const TlsConfig& config) {
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                    ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
    boost::system::error_code ec;
    ctx.set_default_verify_paths(ec);
    if (ec) {
        throw IoError(
            "cannot load the system CA certificates (install the ca-certificates "
            "package): " +
            ec.message());
    }
    if (!config.extra_ca_pem.empty()) {
        ctx.add_certificate_authority(
            boost::asio::buffer(config.extra_ca_pem.data(), config.extra_ca_pem.size()), ec);
        if (ec) {
            throw IoError("cannot load the extra CA certificate: " + ec.message());
        }
    }
    ctx.set_verify_mode(ssl::verify_peer);
    return ctx;
}

}  // namespace blc
