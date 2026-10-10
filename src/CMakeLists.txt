configure_file(blc/version.hpp.in "${CMAKE_BINARY_DIR}/generated/blc/version.hpp" @ONLY)

# All capture / book / CSV logic lives in blc_core so the CLI and the unit tests share one
# implementation. It becomes a static library as modules are added.
add_library(blc_core INTERFACE)
target_include_directories(blc_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}/generated")
target_compile_features(blc_core INTERFACE cxx_std_17)

add_executable(binance_capture main.cpp)
target_link_libraries(binance_capture PRIVATE blc_core)
blc_set_warnings(binance_capture)

# The README and reviewer checklist invoke ./build/binance_capture.
set_target_properties(binance_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
