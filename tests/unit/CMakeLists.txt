# The schema sweep tables, generated into this build directory, are what
# `SchemaJson.MatchesTheOracleOnTheSweepTable` and `SchemaLinker.MatchesTheOracleOnTheSweepTable` run
# when the reference CLI and checkout are set.
set(schemaJsonSweepEnvironment "")
set(schemaLinkSweepEnvironment "")
if(NOT CedarCpp_ReferenceCedarExecutable STREQUAL "" AND NOT CedarCpp_ReferenceSourceDirectory STREQUAL "")
    include(SchemaSweepTables)
    CedarCpp_GenerateSchemaSweepTables(cedar-cpp-schema-sweep-tables "${CMAKE_CURRENT_BINARY_DIR}/schema-sweep")
    set(schemaJsonSweepEnvironment "CEDAR_SCHEMA_JSON_SWEEP_TABLE=${CMAKE_CURRENT_BINARY_DIR}/schema-sweep/SchemaJson.tsv")
    set(schemaLinkSweepEnvironment "CEDAR_SCHEMA_LINK_SWEEP_TABLE=${CMAKE_CURRENT_BINARY_DIR}/schema-sweep/SchemaLink.tsv")
endif()

CedarCpp_AddUnitTest(DiagnosticTests.cpp)
CedarCpp_AddUnitTest(EntityJsonTests.cpp)
target_compile_definitions(cedar-cpp-EntityJsonTests PRIVATE
    CEDAR_JSON_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/json")
CedarCpp_AddUnitTest(ExtensionTests.cpp)
# The extension types are internal; their tests include lib/source/extensions directly.
target_include_directories(cedar-cpp-ExtensionTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
CedarCpp_AddUnitTest(ExpressionParserTests.cpp)
target_compile_definitions(cedar-cpp-ExpressionParserTests PRIVATE
    CEDAR_EXPRESSION_PARITY_TABLE="${PROJECT_SOURCE_DIR}/tests/parity/expressions/Expressions.tsv")
CedarCpp_AddUnitTest(LexerTests.cpp)
# The lexer is internal; its tests include lib/source/syntax directly.
target_include_directories(cedar-cpp-LexerTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
CedarCpp_AddUnitTest(NamesTests.cpp)
CedarCpp_AddUnitTest(PolicyParserTests.cpp)
# The canonical policy-set printer it compares with is internal (lib/source/syntax/Printer.hpp).
target_include_directories(cedar-cpp-PolicyParserTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-PolicyParserTests PRIVATE
    CEDAR_POLICY_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/policies")
CedarCpp_AddUnitTest(PolicySetTests.cpp)
CedarCpp_AddUnitTest(PolicySetApiTests.cpp)
CedarCpp_AddUnitTest(PrinterTests.cpp)
CedarCpp_AddUnitTest(TransitiveClosureTests.cpp)
CedarCpp_AddUnitTest(ValueOrderingTests.cpp)
CedarCpp_AddUnitTest(EvaluatorTests.cpp)
# The evaluator is internal; its tests include lib/source/evaluation directly.
target_include_directories(cedar-cpp-EvaluatorTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-EvaluatorTests PRIVATE
    CEDAR_EVALUATION_PARITY_TABLE="${PROJECT_SOURCE_DIR}/tests/parity/evaluation/Expressions.tsv"
    CEDAR_EVALUATION_ENTITIES="${PROJECT_SOURCE_DIR}/tests/parity/evaluation/Entities.json"
    CEDAR_EVALUATION_CONTEXT="${PROJECT_SOURCE_DIR}/tests/parity/evaluation/Context.json")
CedarCpp_AddUnitTest(AuthorizerTests.cpp)
target_compile_definitions(cedar-cpp-AuthorizerTests PRIVATE
    CEDAR_AUTHORIZATION_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/authorization")
CedarCpp_AddUnitTest(ScopeIndexTests.cpp)
# The workload requests are split with the internal JSON reader (lib/source/json).
target_include_directories(cedar-cpp-ScopeIndexTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-ScopeIndexTests PRIVATE
    CEDAR_AUTHORIZATION_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/authorization")
# The benchmark workloads, generated into this build directory, when the reference checkout is set.
if(NOT CedarCpp_ReferenceSourceDirectory STREQUAL "")
    include(BenchmarkWorkloads)
    CedarCpp_GenerateBenchmarkWorkloads(cedar-cpp-ScopeIndexTests-workloads "${CMAKE_CURRENT_BINARY_DIR}/workloads")
    add_dependencies(cedar-cpp-ScopeIndexTests cedar-cpp-ScopeIndexTests-workloads)
    target_compile_definitions(cedar-cpp-ScopeIndexTests PRIVATE
        CEDAR_BENCHMARK_WORKLOADS_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/workloads")
endif()
CedarCpp_AddUnitTest(PolicyJsonTests.cpp)
# The CLI-style reader and the canonical printer it checks with are internal (lib/source/json, lib/source/syntax).
target_include_directories(cedar-cpp-PolicyJsonTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-PolicyJsonTests PRIVATE
    CEDAR_POLICY_JSON_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/policies-json"
    CEDAR_POLICY_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/policies")
CedarCpp_AddUnitTest(SchemaJsonTests.cpp ${schemaJsonSweepEnvironment})
# The fragment model is internal; its tests include lib/source/schema directly.
target_include_directories(cedar-cpp-SchemaJsonTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-SchemaJsonTests PRIVATE
    CEDAR_SCHEMA_JSON_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/schemas/json")
CedarCpp_AddUnitTest(SchemaParserTests.cpp)
# The fragment model and the parser's depth limit are internal; the tests include lib/source/schema directly.
target_include_directories(cedar-cpp-SchemaParserTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-SchemaParserTests PRIVATE
    CEDAR_SCHEMA_CEDAR_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/schemas/cedar")
CedarCpp_AddUnitTest(SchemaLinkerTests.cpp ${schemaLinkSweepEnvironment})
# The linked schema is internal; its tests include lib/source/schema directly.
target_include_directories(cedar-cpp-SchemaLinkerTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-SchemaLinkerTests PRIVATE
    CEDAR_SCHEMA_LINK_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/schemas/link")
CedarCpp_AddUnitTest(ValidatorTypesTests.cpp)
# The validator's types are internal; their tests include lib/source/validation directly.
target_include_directories(cedar-cpp-ValidatorTypesTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
CedarCpp_AddUnitTest(TypecheckerTests.cpp)
# The typechecker is internal; its tests include lib/source/validation directly.
target_include_directories(cedar-cpp-TypecheckerTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-TypecheckerTests PRIVATE
    CEDAR_VALIDATION_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/validation")
CedarCpp_AddUnitTest(ValidatorTests.cpp)
target_compile_definitions(cedar-cpp-ValidatorTests PRIVATE
    CEDAR_VALIDATION_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/validation"
    CEDAR_VALIDATION_LEVEL_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/validation-level")
CedarCpp_AddUnitTest(StringChecksTests.cpp)
# The generated Unicode tables are internal; the tests include lib/source/validation directly.
target_include_directories(cedar-cpp-StringChecksTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-StringChecksTests PRIVATE
    CEDAR_STRING_CHECKS_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/string-checks")
CedarCpp_AddUnitTest(SchemaJsonConformanceTests.cpp)
# The request cases are split with the internal JSON reader (lib/source/json).
target_include_directories(cedar-cpp-SchemaJsonConformanceTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-SchemaJsonConformanceTests PRIVATE
    CEDAR_JSON_SCHEMA_PARITY_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/parity/json-schema")
CedarCpp_AddUnitTest(FuzzyMatchTests.cpp)
# The fuzzy matcher is internal; its tests include lib/source/model directly.
target_include_directories(cedar-cpp-FuzzyMatchTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
CedarCpp_AddUnitTest(SchemaIntrospectionTests.cpp)
if(TARGET cedar-cpp-schema-sweep-tables)
    add_dependencies(cedar-cpp-SchemaJsonTests cedar-cpp-schema-sweep-tables)
    add_dependencies(cedar-cpp-SchemaLinkerTests cedar-cpp-schema-sweep-tables)
endif()
CedarCpp_AddUnitTest(ResidualTests.cpp)
# The residual model and the typechecker are internal; the tests include lib/source/ directly.
target_include_directories(cedar-cpp-ResidualTests PRIVATE "${PROJECT_SOURCE_DIR}/lib/source")
target_compile_definitions(cedar-cpp-ResidualTests PRIVATE
    CEDAR_PARTIAL_EVALUATION_SCHEMA="${PROJECT_SOURCE_DIR}/tests/parity/partial-evaluation/Evaluation/schema.cedarschema")
