# Warning flags for cedar-cpp's own targets. Third-party targets keep their own flags, so these are
# applied per target rather than through CMAKE_CXX_FLAGS.
function(CedarCpp_SetWarnings target)
    target_compile_options(${target}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            $<$<BOOL:${CedarCpp_WarningsAsErrors}>:-Werror>
    )
endfunction()
