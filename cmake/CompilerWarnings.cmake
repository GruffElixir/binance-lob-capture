# Strict warning set applied to every first-party target. Third-party code (Boost, simdjson,
# GoogleTest) is consumed through SYSTEM include paths and is not affected.
function(blc_set_warnings target)
    set(warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wold-style-cast
        -Wcast-align
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmisleading-indentation)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND warnings -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
    endif()

    if(BLC_WARNINGS_AS_ERRORS)
        list(APPEND warnings -Werror)
    endif()

    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
