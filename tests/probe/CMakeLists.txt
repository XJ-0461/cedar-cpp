# Compiles and runs the C++26 feature probe. A compile failure here means a feature cedar-cpp relies
# on is missing from the active compiler or standard library.
add_executable(cedar-cpp-cpp26-probe Cpp26Probe.cpp)
target_compile_features(cedar-cpp-cpp26-probe PRIVATE cxx_std_26)
set_target_properties(cedar-cpp-cpp26-probe PROPERTIES CXX_EXTENSIONS OFF)
CedarCpp_SetWarnings(cedar-cpp-cpp26-probe)
add_test(NAME Cpp26Probe COMMAND cedar-cpp-cpp26-probe)
