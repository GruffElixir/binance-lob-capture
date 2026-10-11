#pragma once

#include <stdexcept>

namespace blc {

// Startup-only errors. main maps UsageError to exit code 2 and IoError to exit code 1.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct IoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace blc
