# Vendored simdjson single-header amalgamation (see VERSION). The main build needs no network.
add_library(simdjson STATIC simdjson.cpp)
target_include_directories(simdjson SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(simdjson PUBLIC cxx_std_17)
# Errors are handled through simdjson's error codes; exceptions are never used on the hot path.
target_compile_definitions(simdjson PUBLIC SIMDJSON_EXCEPTIONS=0)
