# CedarCpp_GenerateBenchmarkWorkloads(<target> <output directory>): the custom target `target`
# writes the benchmark workloads (scripts/generate-benchmark-workloads.py, from the reference
# checkout's cedar-examples) into `output directory`. The generator draws from a fixed seed; no
# oracle is needed. Needs CedarCpp_ReferenceSourceDirectory.
function(CedarCpp_GenerateBenchmarkWorkloads target outputDirectory)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(generator "${PROJECT_SOURCE_DIR}/scripts/generate-benchmark-workloads.py")
    set(stamp "${outputDirectory}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${Python3_EXECUTABLE}" -I "${generator}"
            --cedar-examples-directory "${CedarCpp_ReferenceSourceDirectory}/cedar-policy-symcc/tests/data/cedar-examples"
            --output-directory "${outputDirectory}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${generator}"
        COMMENT "Generating the benchmark workloads into ${outputDirectory}"
        VERBATIM
    )
    add_custom_target(${target} ALL DEPENDS "${stamp}")
endfunction()
