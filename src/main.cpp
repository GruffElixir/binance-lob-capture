// binance_capture: Binance WebSocket capture (CSV) + local order book (CSV).

#include "blc/cli/options.hpp"
#include "blc/core/errors.hpp"
#include "blc/replay/replay.hpp"
#include "blc/version.hpp"

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        const blc::Options options = blc::parse_options(argc, argv);
        switch (options.mode) {
            case blc::Mode::Help:
                std::cout << blc::usage_text();
                return 0;
            case blc::Mode::Version:
                std::cout << "binance_capture " << blc::kVersion << '\n';
                return 0;
            case blc::Mode::Replay:
                return blc::run_replay(options.replay);
            case blc::Mode::Capture:
                std::cerr << "binance_capture: not implemented yet\n";
                return 2;
        }
    } catch (const blc::UsageError& e) {
        std::cerr << "binance_capture: " << e.what()
                  << "\nRun 'binance_capture --help' for usage.\n";
        return 2;
    } catch (const blc::IoError& e) {
        std::cerr << "binance_capture: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "binance_capture: fatal: " << e.what() << '\n';
        return 1;
    }
    return 1;
}
