# Every public header must compile on its own. One generated translation unit per header includes
# only that header; the object library builds with the rest of the tests, so a header that leans on
# an include it does not make fails the build.
set(CedarCpp_HeaderSources)
foreach(header IN LISTS CedarCpp_PublicHeaders)
    get_filename_component(name "${header}" NAME_WE)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/${name}Alone.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <cedar/${name}.hpp>\n")
    list(APPEND CedarCpp_HeaderSources "${source}")
endforeach()

# One more unit includes every public header in reverse-alphabetical order, so a name one header
# declares at namespace scope that another header's enumerator or member shadows (-Wshadow) shows up
# when the declaring header comes first.
set(CedarCpp_ReverseHeaders ${CedarCpp_PublicHeaders})
list(TRANSFORM CedarCpp_ReverseHeaders REPLACE "^.*/" "")
list(SORT CedarCpp_ReverseHeaders ORDER DESCENDING)
set(CedarCpp_ReverseOrderContent "")
foreach(header IN LISTS CedarCpp_ReverseHeaders)
    string(APPEND CedarCpp_ReverseOrderContent "#include <cedar/${header}>\n")
endforeach()
set(CedarCpp_ReverseOrderSource "${CMAKE_CURRENT_BINARY_DIR}/AllHeadersReverseOrder.cpp")
file(CONFIGURE OUTPUT "${CedarCpp_ReverseOrderSource}" CONTENT "${CedarCpp_ReverseOrderContent}" @ONLY)
list(APPEND CedarCpp_HeaderSources "${CedarCpp_ReverseOrderSource}")

add_library(cedar-cpp-HeaderSelfContainment OBJECT ${CedarCpp_HeaderSources})
target_link_libraries(cedar-cpp-HeaderSelfContainment PRIVATE cedar::cedar)
CedarCpp_SetWarnings(cedar-cpp-HeaderSelfContainment)
