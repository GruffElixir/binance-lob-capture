add_executable(blc_verify blc_verify.cpp)
target_link_libraries(blc_verify PRIVATE blc_core)
blc_set_warnings(blc_verify)
set_target_properties(blc_verify PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
