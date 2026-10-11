#pragma once

#include <string>

namespace blc::test {
struct TestCa {
    std::string ca_pem;
    std::string certificate_pem;
    std::string key_pem;
};
TestCa make_test_ca();
}  // namespace blc::test
