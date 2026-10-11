configure_file(blc/version.hpp.in "${CMAKE_BINARY_DIR}/generated/blc/version.hpp" @ONLY)

# All capture / book / CSV logic lives in blc_core so the CLI and the unit tests share one
# implementation. Networking (Boost.Beast, OpenSSL) lives in blc_net.
add_library(blc_core STATIC
    blc/book/depth5_check.cpp
    blc/book/diff_window.cpp
    blc/book/order_book.cpp
    blc/book/sequence.cpp
    blc/book/symbol_book.cpp
    blc/cli/options.cpp
    blc/core/decimal.cpp
    blc/core/symbols.cpp
    blc/core/time.cpp
    blc/csv/csv_reader.cpp
    blc/csv/csv_writer.cpp
    blc/csv/rows.cpp
    blc/parse/envelope.cpp
    blc/parse/json_workspace.cpp
    blc/parse/payload_parser.cpp
    blc/parse/stream_table.cpp
    blc/pipeline/consumer.cpp
    blc/pipeline/frame_router.cpp
    blc/pipeline/output_files.cpp
    blc/pipeline/processor.cpp
    blc/pipeline/row_format.cpp
    blc/pipeline/snapshot_requester.cpp
    blc/pipeline/spsc_ring.cpp
    blc/replay/replay.cpp
    blc/util/logging.cpp
    blc/verify/verify.cpp)
target_include_directories(blc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")
target_compile_features(blc_core PUBLIC cxx_std_17)
target_link_libraries(blc_core PUBLIC simdjson)
blc_set_warnings(blc_core)

# Networking (Boost.Beast/Asio + OpenSSL); kept out of blc_core so replay and the unit-tested
# pipeline never depend on it.
find_package(Threads REQUIRED)
find_package(Boost 1.74 REQUIRED)
if(MINGW)
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()
find_package(OpenSSL REQUIRED)

add_library(blc_net STATIC
    blc/net/attempt_limiter.cpp
    blc/net/endpoints.cpp)
target_link_libraries(blc_net PUBLIC blc_core Boost::headers OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads)
target_compile_definitions(blc_net PUBLIC BOOST_ASIO_NO_DEPRECATED)
if(WIN32)
    target_link_libraries(blc_net PUBLIC ws2_32 mswsock crypt32)
    target_compile_definitions(blc_net PUBLIC _WIN32_WINNT=0x0A00)
endif()
blc_set_warnings(blc_net)

add_executable(binance_capture main.cpp)
target_link_libraries(binance_capture PRIVATE blc_core)
blc_set_warnings(binance_capture)

# The README and reviewer checklist invoke ./build/binance_capture.
set_target_properties(binance_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
