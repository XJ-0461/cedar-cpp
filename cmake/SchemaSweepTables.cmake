# CedarCpp_GenerateSchemaSweepTables(<target> <output directory>): the custom target `target`
# writes, into `output directory`, a corpus of JSON schema fragments (scripts/generate-schema-json-sweep-corpus.py:
# the reference checkout's and, when set, the integration tests' schemas, their Cedar schemas
# translated to JSON, and seeded structural mutants of those) and the two parity tables over it, as the reference CLI decides them:
#   SchemaJson.tsv   scripts/generate-schema-json-parity-table.sh
#   SchemaLink.tsv   scripts/generate-schema-link-parity-table.sh
# Both list the corpus files by absolute path. Needs CedarCpp_ReferenceCedarExecutable and
# CedarCpp_ReferenceSourceDirectory; CedarCpp_IntegrationTestsDirectory widens the corpus.
function(CedarCpp_GenerateSchemaSweepTables target outputDirectory)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(corpusGenerator "${PROJECT_SOURCE_DIR}/scripts/generate-schema-json-sweep-corpus.py")
    set(jsonGenerator "${PROJECT_SOURCE_DIR}/scripts/generate-schema-json-parity-table.sh")
    set(linkGenerator "${PROJECT_SOURCE_DIR}/scripts/generate-schema-link-parity-table.sh")
    set(corpus "${outputDirectory}/corpus")
    set(inputs --input-directory "${CedarCpp_ReferenceSourceDirectory}")
    if(NOT CedarCpp_IntegrationTestsDirectory STREQUAL "")
        list(APPEND inputs --input-directory "${CedarCpp_IntegrationTestsDirectory}")
    endif()
    set(stamp "${outputDirectory}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${outputDirectory}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${outputDirectory}"
        COMMAND "${Python3_EXECUTABLE}" -I "${corpusGenerator}"
            --oracle "${CedarCpp_ReferenceCedarExecutable}"
            ${inputs}
            --output-directory "${corpus}"
            --mutants 4000 --seed 1
        COMMAND "${jsonGenerator}"
            --oracle "${CedarCpp_ReferenceCedarExecutable}"
            --input-directory "${corpus}"
            --output "${outputDirectory}/SchemaJson.tsv"
        COMMAND "${linkGenerator}"
            --oracle "${CedarCpp_ReferenceCedarExecutable}"
            --input-directory "${corpus}"
            --output "${outputDirectory}/SchemaLink.tsv"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${corpusGenerator}" "${jsonGenerator}" "${linkGenerator}" "${CedarCpp_ReferenceCedarExecutable}"
        COMMENT "Generating the schema sweep tables into ${outputDirectory}"
        VERBATIM
    )
    add_custom_target(${target} ALL DEPENDS "${stamp}")
endfunction()
