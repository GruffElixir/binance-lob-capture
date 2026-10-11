#include "bench.hpp"

#include <exception>
#include <string_view>
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: blc_bench book|payload|decimal|ring|replay [market_data.csv]\n";
        return 2;
    }
    try {
        const std::string_view kind(argv[1]);
        if (kind == "book")
            blc::bench::book();
        else if (kind == "payload")
            blc::bench::payload();
        else if (kind == "decimal")
            blc::bench::decimal();
        else if (kind == "ring")
            blc::bench::ring();
        else if (kind == "replay" && argc == 3)
            blc::bench::replay(argv[2]);
        else
            return 2;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
