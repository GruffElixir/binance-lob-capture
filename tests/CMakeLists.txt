include(FetchContent)

# Extract archives with the current time so rebuilds after a re-download are correct.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Downloaded at configure time (pinned URL + hash). Offline builds: -DBLC_BUILD_TESTS=OFF, or
# -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<local copy>. The binance_capture target needs no network.
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Lands at ./build/tests/unit_tests, the path used by the README and the reviewer checklist.
add_executable(unit_tests
    smoke_test.cpp
    core_time_test.cpp
    core_instrument_id_test.cpp
    core_decimal_test.cpp
    csv_schema_test.cpp
    csv_writer_test.cpp
    csv_reader_test.cpp
    core_symbols_test.cpp
    cli_options_test.cpp
    simdjson_smoke_test.cpp
    parse_envelope_test.cpp
    parse_stream_table_test.cpp
    parse_payload_test.cpp
    book_side_test.cpp
    book_coverage_test.cpp
    book_sequence_test.cpp
    book_diff_window_test.cpp
    book_depth5_check_test.cpp)
target_link_libraries(unit_tests PRIVATE blc_core GTest::gtest_main)
set(BLC_TEST_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
configure_file(support/paths.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/support/paths.hpp" @ONLY)
target_include_directories(unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
blc_set_warnings(unit_tests)

# `cmake --build build --target tests` is the documented way to build the test suite.
add_custom_target(tests DEPENDS unit_tests)

include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST)
