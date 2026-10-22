# The workloads are generated from the reference checkout's cedar-examples into the build tree, so
# a build of cedar-cpp-benchmarks leaves everything it reads in place except the checked-in
# expected-decisions.json.
if(NOT CedarCpp_ReferenceSourceDirectory)
    message(FATAL_ERROR
        "CedarCpp_BuildBenchmarks needs CedarCpp_ReferenceSourceDirectory: the benchmark workloads are "
        "generated from its cedar-policy-symcc/tests/data/cedar-examples")
endif()
include(BenchmarkWorkloads)
CedarCpp_GenerateBenchmarkWorkloads(cedar-cpp-benchmark-workloads "${CMAKE_CURRENT_BINARY_DIR}/workloads")

add_executable(cedar-cpp-benchmarks Workloads.cpp)
# simdjson reads the workload files (links, requests, expected decisions) outside the timed regions.
target_link_libraries(cedar-cpp-benchmarks PRIVATE cedar::cedar simdjson::simdjson benchmark::benchmark)
add_dependencies(cedar-cpp-benchmarks cedar-cpp-benchmark-workloads)
CedarCpp_SetWarnings(cedar-cpp-benchmarks)
