#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace blc {

struct VerifyOptions {
    std::vector<std::filesystem::path> files;
    std::optional<std::filesystem::path> epoch_log;  // stderr log with epoch_closed lines
};

struct VerifyResult {
    bool ok = true;
    std::vector<std::string> errors;  // at most 100, plus one "N more" line
};

// Checks output files against the CSV contracts (design §3, §8): each file's type comes from its
// header. With an epoch log, also checks the frame-accounting invariant per (shard, epoch).
// Memory is bounded by reader/parser capacity, the number of files and the capped diagnostics.
VerifyResult verify_files(const VerifyOptions& options);

}  // namespace blc
