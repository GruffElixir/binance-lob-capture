#include "blc/book/sequence.hpp"

namespace blc {

SeqClass classify(Venue venue, std::uint64_t U, std::uint64_t u, std::uint64_t pu,
                  std::uint64_t ref) noexcept {
    if (u <= ref) {
        return SeqClass::Stale;
    }
    if (venue == Venue::Spot) {
        return U > ref + 1 ? SeqClass::Gap : SeqClass::Continues;
    }
    return pu != ref ? SeqClass::Gap : SeqClass::Continues;
}

bool contained_in_snapshot(Venue venue, std::uint64_t u, std::uint64_t L) noexcept {
    return venue == Venue::Spot ? u <= L : u < L;
}

bool bridges(Venue venue, std::uint64_t U, std::uint64_t u, std::uint64_t L) noexcept {
    if (venue == Venue::Spot) {
        return U <= L + 1 && L + 1 <= u;
    }
    return U <= L && L <= u;
}

}  // namespace blc
