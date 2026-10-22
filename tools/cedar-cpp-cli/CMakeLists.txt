add_executable(cedar-cpp-cli
    Arguments.cpp
    Authorize.cpp
    CheckParse.cpp
    Commands.cpp
    Evaluate.cpp
    Inputs.cpp
    JsonInputs.cpp
    Link.cpp
    Main.cpp
    Report.cpp
    Requests.cpp
    RunTests.cpp
    SchemaInputs.cpp
    TranslatePolicy.cpp
    TranslateSchema.cpp
    Validate.cpp
)
target_link_libraries(cedar-cpp-cli PRIVATE cedar::cedar)
# The UTF-8 decoder, the JSON syntax tree and the id escaping it uses are internal (source/).
target_include_directories(cedar-cpp-cli PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(cedar-cpp-cli PRIVATE CEDAR_CPP_VERSION="${PROJECT_VERSION}")
CedarCpp_SetWarnings(cedar-cpp-cli)
