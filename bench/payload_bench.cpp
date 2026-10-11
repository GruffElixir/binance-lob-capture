#include "bench.hpp"
#include "blc/parse/envelope.hpp"
#include "blc/parse/payload_parser.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace blc::bench {
void payload() {
    PayloadParser parser;
    EnvelopeSplitter splitter;
    auto diff = PayloadParser::make_diff();
    auto snapshot = PayloadParser::make_snapshot();
    Depth5 depth5{};
    Trade trade{};
    for (const auto venue : {Venue::Spot, Venue::Usdm})
        for (const std::string kind : {"diff", "depth5", "trade", "snapshot"}) {
            const auto name = std::string(to_string(venue)) + "_" + kind;
            std::ifstream file(std::filesystem::path(BLC_BENCH_SOURCE_DIR) /
                               "tests/fixtures/payloads" / (name + ".json"));
            if (!file)
                throw std::runtime_error("missing fixture " + name);
            const std::string json{std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>()};
            const simdjson::padded_string padded(json);
            const PaddedView view{padded.data(), padded.size(),
                                  padded.size() + simdjson::SIMDJSON_PADDING};
            auto decode = [&] {
                if (kind == "diff")
                    return parser.diff(venue, view, "BTCUSDT", diff);
                if (kind == "depth5")
                    return parser.depth5(venue, view, "BTCUSDT", depth5);
                if (kind == "trade")
                    return parser.trade(venue, view, "BTCUSDT", trade);
                return parser.snapshot(venue, view, snapshot);
            };
            if (decode() != ParseStatus::Ok)
                throw std::runtime_error("invalid fixture " + name);
            const auto typed_start = Clock::now();
            for (std::size_t i = 0; i < kIterations; ++i)
                if (decode() != ParseStatus::Ok)
                    throw std::runtime_error("decode failed");
            const auto typed = elapsed_ns(typed_start) / static_cast<double>(kIterations);
            const auto frame = "{\"stream\":\"btcusdt@depth@100ms\",\"data\":" + json + "}";
            Envelope envelope;
            const auto network_start = Clock::now();
            for (std::size_t i = 0; i < kIterations; ++i)
                if (!splitter.split(frame, envelope))
                    throw std::runtime_error("split failed");
            const auto network = elapsed_ns(network_start) / static_cast<double>(kIterations);
            std::cout << name << " typed_ns/message=" << typed
                      << " typed_MB/s=" << static_cast<double>(json.size()) * 1000 / typed
                      << " network_ns/message=" << network
                      << " network_MB/s=" << static_cast<double>(frame.size()) * 1000 / network
                      << '\n';
        }
}
}  // namespace blc::bench
