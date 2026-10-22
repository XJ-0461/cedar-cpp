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
    source/entities/Conformance.cpp
    source/entities/Entities.cpp
    source/entities/TransitiveClosure.cpp
    source/evaluation/Authorizer.cpp
    source/evaluation/Evaluator.cpp
    source/evaluation/ScopeIndex.cpp
    source/extensions/ExtensionCalls.cpp
    source/json/ContextJson.cpp
    source/json/EntityJson.cpp
    source/json/JsonReader.cpp
    source/json/JsonWriter.cpp
    source/json/PolicyJson.cpp
    source/json/ValueJson.cpp
    source/model/Diagnostic.cpp
    source/model/Errors.cpp
    source/model/EscapeDebug.cpp
    source/model/EscapeDebugTables.cpp
    source/model/FuzzyMatch.cpp
    source/model/Expression.cpp
    source/model/Names.cpp
    source/model/Policy.cpp
    source/model/PolicySet.cpp
    source/model/Request.cpp
    source/model/SymbolTable.cpp
    source/model/Value.cpp
    source/schema/SchemaCedarPrinter.cpp
    source/schema/SchemaFragmentModel.cpp
    source/schema/SchemaJsonReader.cpp
    source/schema/SchemaJsonWriter.cpp
    source/schema/SchemaLinker.cpp
    source/schema/SchemaParser.cpp
    source/schema/RequestValidation.cpp
    source/schema/ValidatorSchema.cpp
    source/syntax/Folding.cpp
    source/syntax/Parser.cpp
    source/syntax/Printer.cpp
    source/validation/Capabilities.cpp
    source/validation/LevelValidation.cpp
    source/validation/Rbac.cpp
    source/validation/RequestEnvironments.cpp
    source/validation/StringChecks.cpp
    source/validation/Typechecker.cpp
    source/validation/Types.cpp
    source/validation/UnicodeSecurityTables.cpp
    source/validation/Validator.cpp
)
add_library(cedar::cedar ALIAS cedar)

target_compile_features(cedar PUBLIC cxx_std_26)
set_target_properties(cedar PROPERTIES CXX_EXTENSIONS OFF EXPORT_NAME cedar)

# Every installed header, not a subset: a header missing from this list leaves the installed package
# unable to compile any consumer that reaches it through the umbrella.
set(CedarCpp_PublicHeaders
    include/cedar/Authorizer.hpp
    include/cedar/Cedar.hpp
    include/cedar/Diagnostic.hpp
    include/cedar/Entities.hpp
    include/cedar/Errors.hpp
    include/cedar/Expression.hpp
    include/cedar/ExtensionValues.hpp
    include/cedar/Json.hpp
    include/cedar/Names.hpp
    include/cedar/Policy.hpp
    include/cedar/PolicySet.hpp
    include/cedar/Request.hpp
    include/cedar/Schema.hpp
    include/cedar/Syntax.hpp
    include/cedar/Validator.hpp
    include/cedar/Value.hpp
)
target_sources(cedar
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES ${CedarCpp_PublicHeaders}
)

target_include_directories(cedar PRIVATE source)

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
