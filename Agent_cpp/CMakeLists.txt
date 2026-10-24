cmake_minimum_required(VERSION 3.20)
project(data_agent_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Scarica automaticamente i binari ufficiali di DuckDB 
FetchContent_Declare(
    duckdb_bin
    URL https://github.com/duckdb/duckdb/releases/download/v1.4.0/libduckdb-linux-amd64.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(duckdb_bin)

add_library(duckdb SHARED IMPORTED GLOBAL)
set_target_properties(duckdb PROPERTIES
    IMPORTED_LOCATION "${duckdb_bin_SOURCE_DIR}/libduckdb.so"
    INTERFACE_INCLUDE_DIRECTORIES "${duckdb_bin_SOURCE_DIR}"
)

# Rileva agents.cpp o agents-sdk
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../agents.cpp")
    set(AGENTS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../agents.cpp")
else(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../agents-sdk")
    set(AGENTS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../agents-sdk")
endif()

set(AGENTS_SDK_LIB "${AGENTS_SDK_DIR}/lib/linux/x64/libagents_cpp_shared_lib.so")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AGENTS_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json
)


add_executable(data_agent_cpp 
    src/main.cpp 
    src/schema.cpp
    src/steps.cpp 
    src/utils.cpp
    src/data_agent.cpp
)

target_link_libraries(data_agent_cpp PRIVATE
    duckdb
    ${AGENTS_SDK_LIB}
    yaml-cpp
    pthread
    dl
)

set_target_properties(data_agent_cpp PROPERTIES
    BUILD_RPATH "${AGENTS_SDK_DIR}/lib/linux/x64;${duckdb_bin_SOURCE_DIR}"
    INSTALL_RPATH "${AGENTS_SDK_DIR}/lib/linux/x64;${duckdb_bin_SOURCE_DIR}"
)