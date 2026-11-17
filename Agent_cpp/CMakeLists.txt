cmake_minimum_required(VERSION 3.20)
project(data_agent_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Architettura: x86_64 (amd64) oppure ARM64 (aarch64, es. NVIDIA GB10)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(DUCKDB_ARCH "arm64")
    set(AGENTS_LIB_CANDIDATES arm64 aarch64 linux-arm64)
else()
    set(DUCKDB_ARCH "amd64")
    set(AGENTS_LIB_CANDIDATES x64 x86_64 amd64)
endif()
message(STATUS "Architettura rilevata: ${CMAKE_SYSTEM_PROCESSOR} -> DuckDB ${DUCKDB_ARCH}")

# ---------------------------------------------------------------------------
# DuckDB: scarica i binari ufficiali per l'architettura corretta
# ---------------------------------------------------------------------------
FetchContent_Declare(
    duckdb_bin
    URL https://github.com/duckdb/duckdb/releases/download/v1.4.0/libduckdb-linux-${DUCKDB_ARCH}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(duckdb_bin)

add_library(duckdb SHARED IMPORTED GLOBAL)
set_target_properties(duckdb PROPERTIES
    IMPORTED_LOCATION "${duckdb_bin_SOURCE_DIR}/libduckdb.so"
    INTERFACE_INCLUDE_DIRECTORIES "${duckdb_bin_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# agents.cpp / agents-sdk
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../agents.cpp")
    set(AGENTS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../agents.cpp")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../agents-sdk")
    set(AGENTS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../agents-sdk")
else()
    message(FATAL_ERROR "agents.cpp / agents-sdk non trovato in ${CMAKE_CURRENT_SOURCE_DIR}/../../")
endif()

# Permette di forzare la cartella a mano: cmake -B build -DAGENTS_LIB_DIR=/percorso
if(NOT AGENTS_LIB_DIR)
    foreach(cand IN LISTS AGENTS_LIB_CANDIDATES)
        if(EXISTS "${AGENTS_SDK_DIR}/lib/linux/${cand}/libagents_cpp_shared_lib.so")
            set(AGENTS_LIB_DIR "${AGENTS_SDK_DIR}/lib/linux/${cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT AGENTS_LIB_DIR OR NOT EXISTS "${AGENTS_LIB_DIR}/libagents_cpp_shared_lib.so")
    message(FATAL_ERROR
        "libagents_cpp_shared_lib.so per ${CMAKE_SYSTEM_PROCESSOR} non trovata.\n"
        "Cercata in: ${AGENTS_SDK_DIR}/lib/linux/{${AGENTS_LIB_CANDIDATES}}\n"
        "La libreria x64 non funziona su ARM: serve una build per ${CMAKE_SYSTEM_PROCESSOR}.\n"
        "Se ce l'hai altrove, passa -DAGENTS_LIB_DIR=/cartella/che/la/contiene")
endif()
set(AGENTS_SDK_LIB "${AGENTS_LIB_DIR}/libagents_cpp_shared_lib.so")
message(STATUS "agents.cpp lib: ${AGENTS_SDK_LIB}")

# ---------------------------------------------------------------------------
# yaml-cpp: prima cerca un'installazione in ~/.local (senza sudo), poi il sistema
# ---------------------------------------------------------------------------
find_package(yaml-cpp CONFIG QUIET HINTS "$ENV{HOME}/.local")
if(TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(YAML_CPP_TARGET yaml-cpp)
else()
    set(YAML_CPP_TARGET yaml-cpp)   # fallback: libreria di sistema
endif()
message(STATUS "yaml-cpp: ${YAML_CPP_TARGET} (${yaml-cpp_DIR})")

# ---------------------------------------------------------------------------
# Eseguibile
# ---------------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AGENTS_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/difflib
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
    ${YAML_CPP_TARGET}
    pthread
    dl
)

set_target_properties(data_agent_cpp PROPERTIES
    BUILD_RPATH "${AGENTS_LIB_DIR};${duckdb_bin_SOURCE_DIR}"
    INSTALL_RPATH "${AGENTS_LIB_DIR};${duckdb_bin_SOURCE_DIR}"
)