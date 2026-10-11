#include "bench.hpp"
#include "blc/book/book_side.hpp"

#include <cstdint>
#include <map>
namespace blc::bench {
namespace {
volatile std::int64_t checksum = 0;
struct MapSide {
    std::map<std::int64_t, std::int64_t> levels;
    void set(std::int64_t price, std::int64_t qty) {
        if (qty == 0)
            levels.erase(price);
        else
            levels[price] = qty;
    }
    std::int64_t best() const { return levels.rbegin()->second; }
};
struct FlatSide : BookSide<Side::Bid> {
    std::int64_t best() const { return BookSide<Side::Bid>::best(0).qty; }
};
template <class Book>
std::vector<double> measure(std::int64_t n, bool deep, bool erase) {
    std::vector<double> times;
    for (int repeat = 0; repeat < 10; ++repeat) {
        Book side;
        for (std::int64_t p = 1; p <= n; ++p)
            side.set(p, 1);
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kIterations; ++i) {
            const auto offset = static_cast<std::int64_t>((i / (erase ? 2u : 1u)) % 5);
            const auto price = deep ? 1 + offset : n - offset;
            side.set(price, erase && i % 2 == 0 ? 0 : static_cast<std::int64_t>(i % 7 + 1));
        }
        times.push_back(elapsed_ns(start) / static_cast<double>(kIterations));
        checksum = side.best();
    }
    return times;
}
}  // namespace
void book() {
    std::cout << "10 repetitions; 1,000,000 mutations each; deletes alternate erase/reinsert.\n";
    for (const auto n : {1000, 5000})
        for (const bool deep : {false, true})
            for (const bool erase : {false, true}) {
                const auto name = std::to_string(n) + " " + (deep ? "deep " : "touch ") +
                                  (erase ? "delete" : "update");
                report("flat " + name, measure<FlatSide>(n, deep, erase));
                report("map " + name, measure<MapSide>(n, deep, erase));
            }
}
}  // namespace blc::bench
