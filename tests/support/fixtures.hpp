#pragma once

#include "blc/core/types.hpp"
#include "support/paths.hpp"
#include "support/temp_dir.hpp"

#include <simdjson.h>
#include <string>
#include <vector>

namespace blc::test {

inline std::string fixture_path(const std::string& relative) {
    return std::string(kSourceDir) + "/tests/fixtures/" + relative;
}

inline std::string load_fixture(const std::string& relative) {
    return read_file(fixture_path(relative));
}

// Owns a copy of `json` followed by simdjson padding, for in-place parsing.
class PaddedBuffer {
public:
    explicit PaddedBuffer(const std::string& json)
        : bytes_(json.begin(), json.end()), size_(json.size()) {
        bytes_.resize(size_ + simdjson::SIMDJSON_PADDING, '\0');
    }
    PaddedView view() const { return PaddedView{bytes_.data(), size_, bytes_.size()}; }

private:
    std::vector<char> bytes_;
    std::size_t size_;
};

}  // namespace blc::test
