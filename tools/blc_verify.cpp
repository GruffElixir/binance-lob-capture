// blc_verify: checks binance_capture output files against their CSV contracts.
//
//   blc_verify [--epoch-log <stderr-log>] <file>...
//
// Exit status 0 when every check passes, 1 otherwise (2 for a usage error).

#include "blc/verify/verify.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    blc::VerifyOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--epoch-log") {
            if (i + 1 >= argc) {
                std::cerr << "blc_verify: --epoch-log needs a file\n";
                return 2;
            }
            options.epoch_log = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: blc_verify [--epoch-log <stderr-log>] <file>...\n";
            return 0;
        } else {
            options.files.emplace_back(arg);
        }
    }
    if (options.files.empty()) {
        std::cerr << "usage: blc_verify [--epoch-log <stderr-log>] <file>...\n";
        return 2;
    }
    try {
        const blc::VerifyResult result = blc::verify_files(options);
        for (const std::string& e : result.errors) {
            std::cerr << e << '\n';
        }
        std::cout << (result.ok ? "OK" : "FAILED") << ": " << options.files.size() << " file(s)\n";
        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "blc_verify: " << e.what() << '\n';
        return 1;
    }
}
