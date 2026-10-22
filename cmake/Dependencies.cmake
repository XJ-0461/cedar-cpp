# Third-party dependencies. Each is looked up with find_package first (FIND_PACKAGE_ARGS), so a
# consumer whose package manager already provides it wins, and is fetched at a pinned tag otherwise.
# SYSTEM keeps their headers out of cedar-cpp's warning flags.

include(FetchContent)

# simdjson is the only runtime dependency. It is linked privately, but a static cedar still carries
# the link dependency into the installed package, so when cedar-cpp installs itself a fetched simdjson
# installs next to it and CedarCppConfig.cmake finds it again. A cedar-cpp used as a subproject
# installs nothing, simdjson included.
if(CedarCpp_Install)
    set(SIMDJSON_INSTALL ON CACHE BOOL "Install simdjson alongside cedar-cpp")
    # Installs simdjson.h too; without it the installed simdjson package names a header it lacks.
    set(SIMDJSON_SINGLEHEADER ON CACHE BOOL "Install the simdjson header alongside cedar-cpp")
else()
    set(SIMDJSON_INSTALL OFF CACHE BOOL "cedar-cpp is not installing")
endif()
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v5.0.5
    GIT_SHALLOW TRUE
    SYSTEM
    FIND_PACKAGE_ARGS CONFIG
)
FetchContent_MakeAvailable(simdjson)

if(CedarCpp_BuildTests)
    set(INSTALL_GTEST OFF CACHE BOOL "Do not install GoogleTest with cedar-cpp")
    set(BUILD_GMOCK OFF CACHE BOOL "cedar-cpp does not use GoogleMock")
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.18.0
        GIT_SHALLOW TRUE
        SYSTEM
        FIND_PACKAGE_ARGS NAMES GTest
    )
    FetchContent_MakeAvailable(googletest)
endif()

if(CedarCpp_BuildBenchmarks)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Do not build Google Benchmark's own tests")
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Do not install Google Benchmark with cedar-cpp")
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "Do not install Google Benchmark's documentation")
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.10.0
        GIT_SHALLOW TRUE
        SYSTEM
        FIND_PACKAGE_ARGS CONFIG
    )
    FetchContent_MakeAvailable(benchmark)
endif()
