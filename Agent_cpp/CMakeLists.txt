set(AGENTS_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../agents-sdk)
cmake_minimum_required(VERSION 3.20)
project(data_agent_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AGENTS_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json
)

set(AGENTS_SDK_LIB ${AGENTS_SDK_DIR}/lib/linux/x64/libagents_cpp_shared_lib.so)
set(DUCKDB_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb/libduckdb.so)

add_executable(data_agent_cpp 
    src/main.cpp 
    src/schema.cpp
    src/lookup_sales_data.cpp 
    src/analyzing_data.cpp
    src/utils.cpp
)

target_link_libraries(data_agent_cpp PRIVATE
    ${AGENTS_SDK_LIB}
    ${DUCKDB_LIB}
    yaml-cpp
    pthread
    dl
)

set_target_properties(data_agent_cpp PROPERTIES
    BUILD_RPATH "${AGENTS_SDK_DIR}/lib/linux/x64;${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb"
)