#pragma once

#include "blc/parse/json_workspace.hpp"

#include <cstdint>
#include <string_view>

namespace blc {

struct Envelope {
    std::string_view stream;
    std::string_view data;  // the inner "data" object, exactly as received
};

// Splits a combined-stream frame {"stream":"<name>","data":{...}} (design §6.2).
// Every frame is first validated completely as JSON (invalid JSON never reaches a CSV), then the
// canonical layout is split by a byte scan; any other layout goes through a single-pass,
// order-independent fallback. Views point into `frame` (fast path) or into the workspace
// (fallback) and stay valid until the next call.
class EnvelopeSplitter {
public:
    EnvelopeSplitter() = default;

    bool split(std::string_view frame, Envelope& out) noexcept;
    std::uint64_t fallbacks() const noexcept { return fallbacks_; }

    // Shared with the REST client on the io thread (serial use only).
    JsonWorkspace& json_workspace() noexcept { return workspace_; }

private:
    bool fallback(std::string_view frame, Envelope& out) noexcept;

    JsonWorkspace workspace_;
    std::uint64_t fallbacks_ = 0;
};

}  // namespace blc
