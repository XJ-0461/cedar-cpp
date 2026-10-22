add_executable(cedar-cpp-cli
    source/Arguments.cpp
    source/Authorize.cpp
    source/CheckParse.cpp
    source/Commands.cpp
    source/Evaluate.cpp
    source/Inputs.cpp
    source/JsonInputs.cpp
    source/Link.cpp
    source/Main.cpp
    source/PartialEvaluation.cpp
    source/Report.cpp
    source/Requests.cpp
    source/RunTests.cpp
    source/SchemaInputs.cpp
    source/TranslatePolicy.cpp
    source/TranslateSchema.cpp
    source/Validate.cpp
)
target_link_libraries(cedar-cpp-cli PRIVATE cedar::cedar)
# The UTF-8 decoder, the JSON syntax tree and the id escaping the CLI uses are internal to the
# library, so its source/ directory is on the include path next to the CLI's own include/.
target_include_directories(cedar-cpp-cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/source"
)
target_compile_definitions(cedar-cpp-cli PRIVATE CEDAR_CPP_VERSION="${PROJECT_VERSION}")
CedarCpp_SetWarnings(cedar-cpp-cli)
