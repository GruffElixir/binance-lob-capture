// binance_capture: Binance WebSocket capture (CSV) + local order book (CSV).
//
// Scaffold entry point. The capture, order-book and replay pipeline is built milestone by
// milestone; until then the binary only reports its version.

#include "blc/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--version" || arg == "-V") {
            std::cout << "binance_capture " << blc::kVersion << '\n';
            return 0;
        }
    }
    std::cerr << "binance_capture " << blc::kVersion
              << ": capture pipeline not implemented yet (scaffold build)\n";
    return 2;
}
