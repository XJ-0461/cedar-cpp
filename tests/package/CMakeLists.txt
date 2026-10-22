# Installs this build tree into a prefix inside the build directory, then configures, builds and runs
# the consumer project against that prefix with find_package(CedarCpp CONFIG REQUIRED).
add_test(
    NAME PackageConsumer
    COMMAND "${CMAKE_COMMAND}"
        "-DCedarCpp_BinaryDirectory=${PROJECT_BINARY_DIR}"
        "-DConsumerSourceDirectory=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
        "-DWorkDirectory=${CMAKE_CURRENT_BINARY_DIR}/package-consumer"
        "-DGenerator=${CMAKE_GENERATOR}"
        "-DBuildType=${CMAKE_BUILD_TYPE}"
        "-DCxxCompiler=${CMAKE_CXX_COMPILER}"
        "-DCxxFlags=${CMAKE_CXX_FLAGS}"
        "-DExeLinkerFlags=${CMAKE_EXE_LINKER_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunPackageConsumer.cmake"
)
