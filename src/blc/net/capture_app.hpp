#pragma once
#include "blc/cli/options.hpp"
#include "blc/net/endpoints.hpp"
#include "blc/net/tls.hpp"
#include "blc/pipeline/processor.hpp"

#include <chrono>
#include <functional>
#include <memory>
namespace blc {
struct CaptureConfig {
    CaptureOptions options;
    VenueEndpoints endpoints;
    TlsConfig tls;
    NetTuning tuning;
    std::function<std::unique_ptr<SymbolOutput>(const SymbolSpec&)> output_factory;
    std::function<void(std::uint16_t, std::size_t, bool)> buffer_capacity_observer;
};
int run_capture(const CaptureConfig&, std::chrono::steady_clock::time_point started_at);
std::size_t budget_mib(std::size_t symbols, std::size_t connections);
}  // namespace blc
