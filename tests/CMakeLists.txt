include(GoogleTest)

# One gtest executable per test file, named after the file, with each test case registered with
# ctest individually.
function(CedarCpp_AddUnitTest source)
    get_filename_component(name "${source}" NAME_WE)
    set(target "cedar-cpp-${name}")
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE cedar::cedar GTest::gtest_main)
    CedarCpp_SetWarnings(${target})
    gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST)
endfunction()

add_subdirectory(headers)
add_subdirectory(probe)
add_subdirectory(unit)
add_subdirectory(parity)
if(CedarCpp_Install)
    add_subdirectory(package)
endif()
