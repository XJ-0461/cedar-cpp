# CedarCpp_GeneratePartialEvaluationSweep(<target> <output directory>): the custom target `target`
# writes the partial-evaluation sweep (scripts/generate-partial-evaluation-sweep.py) into
# `output directory`: one manifest per variant, `<Variant>.jsonl`, derived from every request of the
# integration corpus, and the partial entities documents those manifests name. No oracle is needed
# to generate it. Needs CedarCpp_IntegrationTestsDirectory.
function(CedarCpp_GeneratePartialEvaluationSweep target outputDirectory)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(generator "${PROJECT_SOURCE_DIR}/scripts/generate-partial-evaluation-sweep.py")
    set(stamp "${outputDirectory}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${outputDirectory}"
        COMMAND "${Python3_EXECUTABLE}" -I "${generator}"
            --integration-tests-directory "${CedarCpp_IntegrationTestsDirectory}"
            --output-directory "${outputDirectory}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${generator}"
        COMMENT "Generating the partial-evaluation sweep into ${outputDirectory}"
        VERBATIM
    )
    add_custom_target(${target} ALL DEPENDS "${stamp}")
endfunction()
