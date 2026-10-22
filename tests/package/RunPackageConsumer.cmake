# Script mode. Every input is passed with -D by tests/package/CMakeLists.txt.
foreach(required CedarCpp_BinaryDirectory ConsumerSourceDirectory WorkDirectory Generator CxxCompiler)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunPackageConsumer.cmake: -D${required}=... is required")
    endif()
endforeach()

set(prefix "${WorkDirectory}/install")
set(consumerBinaryDirectory "${WorkDirectory}/consumer-build")
file(REMOVE_RECURSE "${WorkDirectory}")

function(Run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "Command failed (${result}): ${command}")
    endif()
endfunction()

Run("${CMAKE_COMMAND}" --install "${CedarCpp_BinaryDirectory}" --prefix "${prefix}")

Run("${CMAKE_COMMAND}"
    -S "${ConsumerSourceDirectory}"
    -B "${consumerBinaryDirectory}"
    -G "${Generator}"
    "-DCMAKE_BUILD_TYPE=${BuildType}"
    "-DCMAKE_CXX_COMPILER=${CxxCompiler}"
    "-DCMAKE_CXX_FLAGS=${CxxFlags}"
    "-DCMAKE_EXE_LINKER_FLAGS=${ExeLinkerFlags}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
)

Run("${CMAKE_COMMAND}" --build "${consumerBinaryDirectory}")

Run("${consumerBinaryDirectory}/cedar-cpp-package-consumer")
