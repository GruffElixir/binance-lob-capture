# Applied globally (including GoogleTest) so ASan/UBSan/TSan see every translation unit;
# a partially instrumented TSan build reports false positives.
if(BLC_SANITIZER)
    if(BLC_SANITIZER MATCHES "thread" AND BLC_SANITIZER MATCHES "address")
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
    endif()
    message(STATUS "Sanitizers enabled: ${BLC_SANITIZER}")
    add_compile_options(-fsanitize=${BLC_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${BLC_SANITIZER})
endif()
