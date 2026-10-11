#pragma once

#include <string>

#include <boost/asio/ssl/context.hpp>

namespace blc {

struct TlsConfig {
    std::string extra_ca_pem;  // empty: the system store only (tests inject a loopback CA)
};

// TLS client context (design §6.2): TLS >= 1.2, the system CA store, verify_peer. Throws
// IoError when the CA store cannot be loaded. Hostname verification and SNI are set per stream.
boost::asio::ssl::context make_tls_client_context(const TlsConfig& config);

}  // namespace blc
