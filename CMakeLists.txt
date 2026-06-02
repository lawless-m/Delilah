cmake_minimum_required(VERSION 3.21)

set(TARGET_NAME dbisam)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# DuckDB links the static MSVC runtime (/MT). Our targets must match or the
# link fails with LNK2038 RuntimeLibrary mismatch. CMP0091 (NEW under our
# cmake_minimum_required) makes this abstraction authoritative over flags.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# We only need libduckdb_static to link the extension against — not DuckDB's
# own unittest/benchmark suites. Skipping them speeds the build and, on
# Windows, sidesteps a std::byte vs <rpcndr.h> ::byte collision in DuckDB's
# test_common.cpp that has nothing to do with this extension.
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

set(DUCKDB_VERSION_NORMALIZED "v1.5.2")

FetchContent_Declare(
    duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG ${DUCKDB_VERSION_NORMALIZED}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(duckdb)

# Vendored Blowfish + MD5 — see third_party/{blowfish,md5}/*.hpp.
add_library(dbisam_blowfish STATIC third_party/blowfish/blowfish.cpp)
target_include_directories(dbisam_blowfish PUBLIC third_party/blowfish)

add_library(dbisam_md5 STATIC third_party/md5/md5.cpp)
target_include_directories(dbisam_md5 PUBLIC third_party/md5)

# Protocol layer — pure code, no DuckDB deps. Linked into the extension
# and exercised directly from the test executable.
add_library(dbisam_protocol STATIC
    src/protocol/wire.cpp
    src/protocol/framing.cpp
    src/protocol/sys_socket.cpp
    src/protocol/crypto.cpp
    src/protocol/msg.cpp
    src/protocol/schema.cpp
    src/protocol/cursor_info.cpp
    src/protocol/row.cpp
    src/protocol/response.cpp
    src/protocol/cursor.cpp
    src/protocol/blob.cpp
    src/protocol/text.cpp
    src/protocol/client.cpp
)
if(WIN32)
    target_link_libraries(dbisam_protocol PUBLIC ws2_32)
endif()
target_include_directories(dbisam_protocol
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src/include
        ${duckdb_SOURCE_DIR}/third_party/miniz
)
target_link_libraries(dbisam_protocol PUBLIC dbisam_blowfish dbisam_md5 duckdb_miniz)

set(EXTENSION_SOURCES
    src/dbisam_extension.cpp
    src/dbisam_scan.cpp
    src/dbisam_storage.cpp
    src/storage/dbisam_attach_options.cpp
    src/storage/dbisam_catalog.cpp
    src/storage/dbisam_filter_render.cpp
    src/storage/dbisam_optimizer.cpp
    src/storage/dbisam_schema_entry.cpp
    src/storage/dbisam_table_entry.cpp
    src/storage/dbisam_transaction.cpp
    src/storage/dbisam_transaction_manager.cpp
)

add_library(${LOADABLE_EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})

set_target_properties(${LOADABLE_EXTENSION_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    OUTPUT_NAME ${TARGET_NAME}
    PREFIX ""
    SUFFIX ".duckdb_extension"
)

target_include_directories(${LOADABLE_EXTENSION_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/include
        ${duckdb_SOURCE_DIR}/src/include
)

target_compile_definitions(${LOADABLE_EXTENSION_NAME}
    PUBLIC -DDUCKDB_BUILD_LOADABLE_EXTENSION
)

target_link_libraries(${LOADABLE_EXTENSION_NAME}
    PRIVATE
        dbisam_protocol
        duckdb_static
        dummy_static_extension_loader
)
# GNU-ld-only linker flags; MSVC and other linkers reject them. The
# upstream DuckDB build helper applies the equivalent flags itself
# when EXTENSION_STATIC_BUILD is on for those toolchains.
if(NOT WIN32 AND NOT APPLE)
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
    )
endif()
target_include_directories(${LOADABLE_EXTENSION_NAME}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

add_custom_command(
    TARGET ${LOADABLE_EXTENSION_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DABI_TYPE=CPP
        -DEXTENSION=$<TARGET_FILE:${LOADABLE_EXTENSION_NAME}>
        -DPLATFORM_FILE=${duckdb_BINARY_DIR}/duckdb_platform_out
        -DVERSION_FIELD=${DUCKDB_VERSION_NORMALIZED}
        -DEXTENSION_VERSION=${DUCKDB_VERSION_NORMALIZED}
        -DNULL_FILE=${duckdb_SOURCE_DIR}/scripts/null.txt
        -P ${duckdb_SOURCE_DIR}/scripts/append_metadata.cmake
)
add_dependencies(${LOADABLE_EXTENSION_NAME} duckdb_platform)

# Standalone test executable — no DuckDB linkage, just the protocol lib.
enable_testing()
add_executable(test_phase2 test/test_phase2.cpp)
target_link_libraries(test_phase2 PRIVATE dbisam_protocol)
add_test(NAME phase2 COMMAND test_phase2)

add_executable(test_phase3 test/test_phase3.cpp)
target_link_libraries(test_phase3 PRIVATE dbisam_protocol)
add_test(NAME phase3 COMMAND test_phase3)

add_executable(test_phase4 test/test_phase4.cpp)
target_link_libraries(test_phase4 PRIVATE dbisam_protocol)
add_test(NAME phase4 COMMAND test_phase4)

add_executable(test_phase4_live test/test_phase4_live.cpp)
target_link_libraries(test_phase4_live PRIVATE dbisam_protocol)

add_executable(test_phase5 test/test_phase5.cpp)
target_link_libraries(test_phase5 PRIVATE dbisam_protocol)
add_test(NAME phase5 COMMAND test_phase5)

add_executable(test_phase5_live test/test_phase5_live.cpp)
target_link_libraries(test_phase5_live PRIVATE dbisam_protocol)

add_executable(test_phase5_dump test/test_phase5_dump.cpp)
target_link_libraries(test_phase5_dump PRIVATE dbisam_protocol)

add_executable(test_phase7 test/test_phase7.cpp)
target_link_libraries(test_phase7 PRIVATE dbisam_protocol)
add_test(NAME phase7 COMMAND test_phase7)

add_executable(test_text test/test_text.cpp)
target_link_libraries(test_text PRIVATE dbisam_protocol)
add_test(NAME text COMMAND test_text)

# parse_attach_path lives in src/storage/ (not the protocol layer because
# it includes the duckdb-facing dbisam/storage/* headers). Build directly
# rather than via dbisam_protocol.
add_executable(test_attach_options
    test/test_attach_options.cpp
    src/storage/dbisam_attach_options.cpp
)
target_link_libraries(test_attach_options PRIVATE dbisam_protocol)
target_include_directories(test_attach_options PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
)
add_test(NAME attach_options COMMAND test_attach_options)
