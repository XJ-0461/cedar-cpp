# The integration-test runner (Rust's `perform_integration_test`). With --require-all-pass it exits 1
# when any file fails or has its validation skipped; without it, 0 whatever the results.
add_executable(cedar-cpp-corpus-runner CorpusRunner.cpp)
target_link_libraries(cedar-cpp-corpus-runner PRIVATE cedar::cedar)
# The JsonTest descriptors are read with the internal JSON syntax tree (source/json).
target_include_directories(cedar-cpp-corpus-runner PRIVATE "${PROJECT_SOURCE_DIR}/source")
CedarCpp_SetWarnings(cedar-cpp-corpus-runner)

# The hand-written tests, the DRT corpus and the regression cases must all pass, under both policy
# selections. Each run lists its failures in CorpusRunnerFailures-<selection>.tsv.
if(NOT CedarCpp_IntegrationTestsDirectory STREQUAL "")
    set(corpusRunnerRegressions "")
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Regressions")
        set(corpusRunnerRegressions --regressions-directory "${CMAKE_CURRENT_SOURCE_DIR}/Regressions")
    endif()
    foreach(name IN ITEMS Every Index)
        string(TOLOWER "${name}" selection)
        add_test(NAME CorpusRunner.AllPass.${name}
            COMMAND cedar-cpp-corpus-runner
                --integration-tests-directory "${CedarCpp_IntegrationTestsDirectory}"
                ${corpusRunnerRegressions}
                --policy-selection ${selection}
                --failures-output "${CMAKE_CURRENT_BINARY_DIR}/CorpusRunnerFailures-${selection}.tsv"
                --require-all-pass
        )
    endforeach()
endif()

# Differential tests against the reference Rust CLI (OracleDiff.sh). Each is registered only when
# cedar-cpp-cli is built and the paths it needs are set: CedarCpp_ReferenceCedarExecutable, and the
# directory it reads.

if(NOT CedarCpp_BuildTools OR CedarCpp_ReferenceCedarExecutable STREQUAL "")
    return()
endif()

# As CedarCpp_AddOracleDiffTest, against the reference CLI `reference`. A command that authorizes
# also gets `<name>.Index`, the same sweep with cedar-cpp-cli selecting policies through the scope
# index.
function(CedarCpp_AddOracleDiffTestAgainst name reference command)
    set(directories "")
    foreach(directory IN LISTS ARGN)
        list(APPEND directories --input-directory "${directory}")
    endforeach()
    set(names ${name})
    if(command STREQUAL "authorize" OR command STREQUAL "authorize-schema" OR command STREQUAL "run-tests")
        list(APPEND names ${name}.Index)
    endif()
    foreach(test IN LISTS names)
        add_test(NAME ${test}
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/OracleDiff.sh"
                --reference-cedar "${reference}"
                --cedar-cpp-cli "$<TARGET_FILE:cedar-cpp-cli>"
                ${directories}
                --command ${command}
        )
        set(selection every)
        if(test MATCHES "\\.Index$")
            set(selection index)
        endif()
        # A sanitizer report exits 1 by default, which reads as a rejection; give it a status of its
        # own.
        set_tests_properties(${test} PROPERTIES ENVIRONMENT
            "ASAN_OPTIONS=exitcode=86;UBSAN_OPTIONS=exitcode=86;CEDAR_CPP_POLICY_SELECTION=${selection}")
    endforeach()
endfunction()

function(CedarCpp_AddOracleDiffTest name command)
    CedarCpp_AddOracleDiffTestAgainst(${name} "${CedarCpp_ReferenceCedarExecutable}" ${command} ${ARGN})
endfunction()

# The CLI's `@id` renaming and input checks, which no fixture directory exercises.
CedarCpp_AddOracleDiffTest(OracleDiff.CheckParse.Cases check-parse "${CMAKE_CURRENT_SOURCE_DIR}/check-parse")
# The request-json, context and template-linked inputs, and sandbox_c's linked templates
# (`sandbox-c-linked/linked` is what the sandbox's README has `cedar link` write).
CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.Cases authorize "${CMAKE_CURRENT_SOURCE_DIR}/authorize")
CedarCpp_AddOracleDiffTest(OracleDiff.RunTests.Cases run-tests "${CMAKE_CURRENT_SOURCE_DIR}/run-tests")
# Authorization with a schema: the crafted App schema with entities, contexts and requests that
# use every implicit form; and sandbox_a with its action entities removed, so that its entities
# conform to its schema.
CedarCpp_AddOracleDiffTest(OracleDiff.AuthorizeSchema.Cases authorize-schema "${CMAKE_CURRENT_SOURCE_DIR}/authorize-schema")
CedarCpp_AddOracleDiffTest(OracleDiff.AuthorizeSchema.SandboxA authorize-schema
    "${CMAKE_CURRENT_SOURCE_DIR}/authorize-schema-sandbox-a")
# The crafted schemas: Cedar syntax in all three directions, and the JSON schemas to Cedar.
CedarCpp_AddOracleDiffTest(OracleDiff.TranslateSchema.Cases translate-schema "${CMAKE_CURRENT_SOURCE_DIR}/schemas/cedar")
CedarCpp_AddOracleDiffTest(OracleDiff.TranslateSchema.JsonCases translate-schema-json
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/json")
# The crafted JSON policies (serde's field, tag and sequence rules), to Cedar text.
CedarCpp_AddOracleDiffTest(OracleDiff.TranslatePolicy.Cases translate-policy "${CMAKE_CURRENT_SOURCE_DIR}/policies-json")
# The crafted `link` cases: slot and argument errors, id clashes, template-linked file contents, and
# templates without slots in a JSON policy set.
CedarCpp_AddOracleDiffTest(OracleDiff.Link.Cases link "${CMAKE_CURRENT_SOURCE_DIR}/link")
# The validation parity tables (their strict rows) through the CLIs, and policies generated for
# their schemas. Permissive validation needs a reference CLI built with `permissive-validate`.
CedarCpp_AddOracleDiffTest(OracleDiff.Validate.Cases validate "${CMAKE_CURRENT_SOURCE_DIR}/validation")
# The crafted level validation table (`--level` per row).
CedarCpp_AddOracleDiffTest(OracleDiff.Validate.LevelCases validate "${CMAKE_CURRENT_SOURCE_DIR}/validation-level")
if(NOT CedarCpp_ReferencePermissiveCedarExecutable STREQUAL "")
    CedarCpp_AddOracleDiffTestAgainst(OracleDiff.ValidatePermissive.Cases "${CedarCpp_ReferencePermissiveCedarExecutable}"
        validate-permissive "${CMAKE_CURRENT_SOURCE_DIR}/validation")
endif()

if(NOT CedarCpp_ReferenceSourceDirectory STREQUAL "")
    CedarCpp_AddOracleDiffTest(OracleDiff.CheckParse check-parse "${CedarCpp_ReferenceSourceDirectory}")
    CedarCpp_AddOracleDiffTest(OracleDiff.TranslateSchema translate-schema "${CedarCpp_ReferenceSourceDirectory}")
    # Every `.cedar` file to JSON and back, and every `.cedar.json` file (the translate-policy,
    # json-check-parse and json-authorize samples among them) to Cedar text.
    CedarCpp_AddOracleDiffTest(OracleDiff.TranslatePolicy translate-policy "${CedarCpp_ReferenceSourceDirectory}")

    # The CLI's sample data without a schema.
    set(samples "${CedarCpp_ReferenceSourceDirectory}/cedar-policy-cli/sample-data")
    CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.SandboxA authorize "${samples}/sandbox_a")
    CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.SandboxB authorize "${samples}/sandbox_b")
    CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.SandboxC authorize "${samples}/sandbox_c")
    # The `cargo run link` commands of sandbox_c's README, in order.
    CedarCpp_AddOracleDiffTest(OracleDiff.Link.SandboxC link "${samples}/sandbox_c")
    file(GLOB tinySandboxes LIST_DIRECTORIES true "${samples}/tiny_sandboxes/sample*")
    CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.TinySandboxes authorize ${tinySandboxes})
    CedarCpp_AddOracleDiffTest(OracleDiff.Authorize.JsonAuthorize authorize "${samples}/tiny_sandboxes/json-authorize")
    CedarCpp_AddOracleDiffTest(OracleDiff.AuthorizeSchema.SandboxC authorize-schema "${samples}/sandbox_c")
    CedarCpp_AddOracleDiffTest(OracleDiff.AuthorizeSchema.TinySandboxes authorize-schema ${tinySandboxes})
    CedarCpp_AddOracleDiffTest(OracleDiff.RunTests.TinySandboxes run-tests "${samples}/tiny_sandboxes")

    # Validation: the eight cedar-examples applications (policies with their schema), the ffi
    # test schemas (no policies beside them, so a trivial and a generated policy set each), and
    # every policy file of the CLI's sample data with each schema beside it (the READMEs'
    # `validate` commands among them).
    set(cedarExamples "${CedarCpp_ReferenceSourceDirectory}/cedar-policy-symcc/tests/data/cedar-examples")
    set(ffiSchemas "${CedarCpp_ReferenceSourceDirectory}/cedar-policy/src/ffi/test_schemas")
    CedarCpp_AddOracleDiffTest(OracleDiff.Validate.CedarExamples validate "${cedarExamples}")
    CedarCpp_AddOracleDiffTest(OracleDiff.Validate.FfiSchemas validate "${ffiSchemas}")
    CedarCpp_AddOracleDiffTest(OracleDiff.Validate.SampleData validate "${samples}")
    # The level validation sample (its README validates at levels 0 to 2), at levels 0 to 4.
    CedarCpp_AddOracleDiffTest(OracleDiff.Validate.LevelValidation validate-level
        "${samples}/tiny_sandboxes/level-validation")
    if(NOT CedarCpp_ReferencePermissiveCedarExecutable STREQUAL "")
        set(permissive "${CedarCpp_ReferencePermissiveCedarExecutable}")
        CedarCpp_AddOracleDiffTestAgainst(OracleDiff.ValidatePermissive.CedarExamples "${permissive}" validate-permissive
            "${cedarExamples}")
        CedarCpp_AddOracleDiffTestAgainst(OracleDiff.ValidatePermissive.FfiSchemas "${permissive}" validate-permissive
            "${ffiSchemas}")
        CedarCpp_AddOracleDiffTestAgainst(OracleDiff.ValidatePermissive.SampleData "${permissive}" validate-permissive
            "${samples}")
    endif()
endif()

if(NOT CedarCpp_IntegrationTestsDirectory STREQUAL "")
    CedarCpp_AddOracleDiffTest(OracleDiff.CheckParse.IntegrationTests check-parse
        "${CedarCpp_IntegrationTestsDirectory}/tests" "${CedarCpp_IntegrationTestsDirectory}/corpus-tests")
    CedarCpp_AddOracleDiffTest(OracleDiff.TranslateSchema.IntegrationTests translate-schema
        "${CedarCpp_IntegrationTestsDirectory}/tests" "${CedarCpp_IntegrationTestsDirectory}/corpus-tests")
    CedarCpp_AddOracleDiffTest(OracleDiff.TranslatePolicy.IntegrationTests translate-policy
        "${CedarCpp_IntegrationTestsDirectory}/tests" "${CedarCpp_IntegrationTestsDirectory}/corpus-tests")
endif()
