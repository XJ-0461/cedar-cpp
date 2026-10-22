cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(
    CedarCpp
    VERSION 0.1.0
    DESCRIPTION "A C++26 implementation of the Cedar policy language"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Configure into a separate build directory.")
endif()

option(CedarCpp_BuildTests "Build the cedar-cpp unit tests and the C++26 probe" ${PROJECT_IS_TOP_LEVEL})
option(CedarCpp_BuildTools "Build cedar-cpp-cli" ${PROJECT_IS_TOP_LEVEL})
option(CedarCpp_Install "Install cedar-cpp, its package configuration and (when fetched) simdjson" ${PROJECT_IS_TOP_LEVEL})
option(CedarCpp_BuildBenchmarks "Build the Google Benchmark harness" OFF)
option(CedarCpp_WarningsAsErrors "Treat compiler warnings in cedar-cpp's own targets as errors" OFF)

# Locations of the parity tools' inputs. None has a default; a parity test that needs one is
# registered only when it is set.
set(CedarCpp_ReferenceCedarExecutable "" CACHE FILEPATH "The reference Rust cedar CLI (scripts/build-reference-cli.sh)")
set(CedarCpp_ReferencePermissiveCedarExecutable "" CACHE FILEPATH
    "The reference Rust cedar CLI built with --features permissive-validate (scripts/build-reference-cli.sh)")
set(CedarCpp_ReferencePartialEvaluationCedarExecutable "" CACHE FILEPATH
    "The reference Rust cedar CLI built with --features tpe (scripts/build-reference-cli.sh)")
set(CedarCpp_ReferencePartialEvaluationOracleExecutable "" CACHE FILEPATH
    "The Rust partial-evaluation oracle (scripts/build-partial-evaluation-oracle.sh)")
set(CedarCpp_ReferenceSourceDirectory "" CACHE PATH "A checkout of the Rust cedar repository, for its fixture directories")
set(CedarCpp_IntegrationTestsDirectory "" CACHE PATH "The cedar-integration-tests checkout (scripts/fetch-integration-tests.sh)")

# cedar-cpp uses no C++ modules, so skip the per-source dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(Warnings)
include(Dependencies)

# ---- Library ----

add_library(cedar
    lib/source/entities/Conformance.cpp
    lib/source/entities/Entities.cpp
    lib/source/entities/TransitiveClosure.cpp
    lib/source/evaluation/Authorizer.cpp
    lib/source/evaluation/Evaluator.cpp
    lib/source/evaluation/Operators.cpp
    lib/source/evaluation/ScopeIndex.cpp
    lib/source/extensions/ExtensionCalls.cpp
    lib/source/json/ContextJson.cpp
    lib/source/json/EntityJson.cpp
    lib/source/json/JsonReader.cpp
    lib/source/json/JsonWriter.cpp
    lib/source/json/PartialEntityJson.cpp
    lib/source/json/PolicyJson.cpp
    lib/source/json/ValueJson.cpp
    lib/source/model/Diagnostic.cpp
    lib/source/model/Errors.cpp
    lib/source/model/EscapeDebug.cpp
    lib/source/model/EscapeDebugTables.cpp
    lib/source/model/FuzzyMatch.cpp
    lib/source/model/Expression.cpp
    lib/source/model/Names.cpp
    lib/source/model/Policy.cpp
    lib/source/model/PolicySet.cpp
    lib/source/model/Request.cpp
    lib/source/model/SymbolTable.cpp
    lib/source/model/Value.cpp
    lib/source/partial-evaluation/BatchedAuthorizer.cpp
    lib/source/partial-evaluation/PartialAuthorizer.cpp
    lib/source/partial-evaluation/PartialEntities.cpp
    lib/source/partial-evaluation/PartialEvaluator.cpp
    lib/source/partial-evaluation/PartialRequest.cpp
    lib/source/partial-evaluation/PermissionQueries.cpp
    lib/source/partial-evaluation/Reauthorization.cpp
    lib/source/partial-evaluation/Residual.cpp
    lib/source/schema/SchemaCedarPrinter.cpp
    lib/source/schema/SchemaFragmentModel.cpp
    lib/source/schema/SchemaJsonReader.cpp
    lib/source/schema/SchemaJsonWriter.cpp
    lib/source/schema/SchemaLinker.cpp
    lib/source/schema/SchemaParser.cpp
    lib/source/schema/RequestValidation.cpp
    lib/source/schema/ValidatorSchema.cpp
    lib/source/syntax/Folding.cpp
    lib/source/syntax/Parser.cpp
    lib/source/syntax/Printer.cpp
    lib/source/validation/Capabilities.cpp
    lib/source/validation/LevelValidation.cpp
    lib/source/validation/Rbac.cpp
    lib/source/validation/RequestEnvironments.cpp
    lib/source/validation/StringChecks.cpp
    lib/source/validation/Typechecker.cpp
    lib/source/validation/Types.cpp
    lib/source/validation/UnicodeSecurityTables.cpp
    lib/source/validation/Validator.cpp
)
add_library(cedar::cedar ALIAS cedar)

target_compile_features(cedar PUBLIC cxx_std_26)
set_target_properties(cedar PROPERTIES CXX_EXTENSIONS OFF EXPORT_NAME cedar)

# Every installed header, not a subset: a header missing from this list leaves the installed package
# unable to compile any consumer that reaches it through the umbrella.
set(CedarCpp_PublicHeaders
    lib/include/cedar/Authorizer.hpp
    lib/include/cedar/Cedar.hpp
    lib/include/cedar/Diagnostic.hpp
    lib/include/cedar/Entities.hpp
    lib/include/cedar/Errors.hpp
    lib/include/cedar/Expression.hpp
    lib/include/cedar/ExtensionValues.hpp
    lib/include/cedar/Json.hpp
    lib/include/cedar/Names.hpp
    lib/include/cedar/PartialEvaluation.hpp
    lib/include/cedar/Policy.hpp
    lib/include/cedar/PolicySet.hpp
    lib/include/cedar/Request.hpp
    lib/include/cedar/Schema.hpp
    lib/include/cedar/Syntax.hpp
    lib/include/cedar/Validator.hpp
    lib/include/cedar/Value.hpp
)
target_sources(cedar
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS lib/include
        FILES ${CedarCpp_PublicHeaders}
)

target_include_directories(cedar PRIVATE lib/source)

# simdjson is an implementation detail: no public header names a simdjson type. For a static cedar the
# link dependency still reaches consumers, which is why the package config finds simdjson again.
target_link_libraries(cedar PRIVATE simdjson::simdjson)

CedarCpp_SetWarnings(cedar)

# ---- Tests, tools, benchmarks ----

if(CedarCpp_BuildTests)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CedarCpp_BuildTools)
    add_subdirectory(tools/cedar-cpp-cli)
endif()

if(CedarCpp_BuildBenchmarks)
    add_subdirectory(benchmarks)
endif()

# ---- Install ----

if(CedarCpp_Install)
    set(CedarCpp_InstallConfigDirectory "${CMAKE_INSTALL_LIBDIR}/cmake/CedarCpp")

    install(
        TARGETS cedar
        EXPORT CedarCppTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(
        EXPORT CedarCppTargets
        NAMESPACE cedar::
        DESTINATION "${CedarCpp_InstallConfigDirectory}"
    )

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/CedarCppConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/CedarCppConfig.cmake"
        INSTALL_DESTINATION "${CedarCpp_InstallConfigDirectory}"
    )

    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/CedarCppConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMinorVersion
    )

    install(
        FILES
            "${PROJECT_BINARY_DIR}/CedarCppConfig.cmake"
            "${PROJECT_BINARY_DIR}/CedarCppConfigVersion.cmake"
        DESTINATION "${CedarCpp_InstallConfigDirectory}"
    )
endif()
