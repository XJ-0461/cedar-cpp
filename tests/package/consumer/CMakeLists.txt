# A downstream project that knows cedar-cpp only through its installed package.
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

project(CedarCppPackageConsumer LANGUAGES CXX)

find_package(CedarCpp CONFIG REQUIRED)

add_executable(cedar-cpp-package-consumer Main.cpp)
target_link_libraries(cedar-cpp-package-consumer PRIVATE cedar::cedar)
