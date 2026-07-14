cmake_minimum_required(VERSION 3.20)

project(database_architecture_lab
    VERSION 1.0.0
    DESCRIPTION "Educational PostgreSQL-style and MySQL-style mini databases"
    LANGUAGES CXX)

option(MINIDB_BUILD_TESTS "Build the MiniDB test suite" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(minidb_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(minidb_common
    src/common/result.cpp
    src/common/sql_parser.cpp
)
target_include_directories(minidb_common PUBLIC ${PROJECT_SOURCE_DIR}/include)
minidb_enable_warnings(minidb_common)

add_library(minidb_postgres
    src/postgres/postgres_database.cpp
)
target_include_directories(minidb_postgres PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_postgres PUBLIC minidb_common)
minidb_enable_warnings(minidb_postgres)

add_library(minidb_mysql
    src/mysql/innodb_engine.cpp
    src/mysql/memory_engine.cpp
    src/mysql/mysql_database.cpp
)
target_include_directories(minidb_mysql PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_mysql PUBLIC minidb_common)
minidb_enable_warnings(minidb_mysql)

add_executable(pg_mini apps/postgres_main.cpp)
target_link_libraries(pg_mini PRIVATE minidb_postgres)
minidb_enable_warnings(pg_mini)

add_executable(mysql_mini apps/mysql_main.cpp)
target_link_libraries(mysql_mini PRIVATE minidb_mysql)
minidb_enable_warnings(mysql_mini)

if(MINIDB_BUILD_TESTS)
    include(CTest)
    add_executable(minidb_tests tests/minidb_tests.cpp)
    target_link_libraries(minidb_tests PRIVATE minidb_postgres minidb_mysql)
    minidb_enable_warnings(minidb_tests)
    add_test(NAME minidb_tests COMMAND minidb_tests)
endif()
