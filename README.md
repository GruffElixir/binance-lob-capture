# binance-lob-capture

C++17 tool that captures Binance public WebSocket market data (`depth@100ms`, `depth5@100ms`,
`trade`) into an audit CSV and maintains a local limit order book, emitting a top-5 snapshot
row to an order-book CSV after every processed event. A replay mode rebuilds the order book
from a saved capture with no network access.

> **Status:** under active development. Build, run, and design documentation will be completed
> for the `v1.0.0` submission tag. The specification is [docs/design.md](docs/design.md); the
> full requirement list and how each item is verified is in
> [docs/requirements.md](docs/requirements.md).

## Toolchain

- Target platform: Ubuntu 22.04, GCC 12 (`g++-12`), C++17. Also built with Clang 15.
- Build system: CMake ≥ 3.22, Ninja (optional).

## Build (scaffold)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12
cmake --build build --parallel
cmake --build build --target tests && ./build/tests/unit_tests
./build/binance_capture --version
```
