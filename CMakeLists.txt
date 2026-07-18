cmake_minimum_required(VERSION 3.20)

project(database_architecture_lab
    VERSION 1.0.0
    DESCRIPTION "Educational relational, document, key-value, and graph mini databases"
    LANGUAGES CXX)

# 默认构建测试；学习时也可以通过 -DMINIDB_BUILD_TESTS=OFF 只构建数据库程序。
option(MINIDB_BUILD_TESTS "Build the MiniDB test suite" ON)

# 项目使用 std::variant、std::visit、unordered_map::contains 等 C++20 特性。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(minidb_enable_warnings target)
    # 对不同编译器启用级别相近的严格警告，并确保 MSVC 按 UTF-8 读取中文源码。
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# 共享层提供关系型值、统一结果格式和最小 SQL parser；MongoDB/Redis/Neo4j 只复用结果格式。
# catalog、事务管理器和存储实现不共享，以便从源码结构观察架构差异。
add_library(minidb_common
    src/common/result.cpp
    src/common/sql_parser.cpp
)
target_include_directories(minidb_common PUBLIC ${PROJECT_SOURCE_DIR}/include)
minidb_enable_warnings(minidb_common)

# PostgreSQL 风格实现：逻辑 backend、heap tuple、MVCC 和 VACUUM。
add_library(minidb_postgres
    src/postgres/postgres_database.cpp
)
target_include_directories(minidb_postgres PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_postgres PUBLIC minidb_common)
minidb_enable_warnings(minidb_postgres)

# MySQL 风格实现：Server SQL 层以及 InnoDB、MEMORY 两个可插拔引擎。
add_library(minidb_mysql
    src/mysql/innodb_engine.cpp
    src/mysql/memory_engine.cpp
    src/mysql/mysql_database.cpp
)
target_include_directories(minidb_mysql PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_mysql PUBLIC minidb_common)
minidb_enable_warnings(minidb_mysql)

# MongoDB 风格实现：独立命令 parser、递归 BSON 文档、collection 和简化快照事务。
add_library(minidb_mongo
    src/mongo/bson.cpp
    src/mongo/mongo_parser.cpp
    src/mongo/mongo_database.cpp
)
target_include_directories(minidb_mongo PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_mongo PUBLIC minidb_common)
minidb_enable_warnings(minidb_mongo)

# Redis 风格实现：string keyspace、原子 INCR 和 MULTI 命令队列。
add_library(minidb_redis
    src/redis/redis_parser.cpp
    src/redis/redis_database.cpp
)
target_include_directories(minidb_redis PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_redis PUBLIC minidb_common)
minidb_enable_warnings(minidb_redis)

# Neo4j 风格实现：属性节点、有类型关系、单跳 Cypher MATCH 和快照事务。
add_library(minidb_neo4j
    src/neo4j/cypher_parser.cpp
    src/neo4j/neo4j_database.cpp
)
target_include_directories(minidb_neo4j PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minidb_neo4j PUBLIC minidb_common)
minidb_enable_warnings(minidb_neo4j)

# 五个独立命令行程序用于对照关系型、文档型、键值型和图数据库架构。
add_executable(pg_mini apps/postgres_main.cpp)
target_link_libraries(pg_mini PRIVATE minidb_postgres)
minidb_enable_warnings(pg_mini)

add_executable(mysql_mini apps/mysql_main.cpp)
target_link_libraries(mysql_mini PRIVATE minidb_mysql)
minidb_enable_warnings(mysql_mini)

add_executable(mongo_mini apps/mongo_main.cpp)
target_link_libraries(mongo_mini PRIVATE minidb_mongo)
minidb_enable_warnings(mongo_mini)

add_executable(redis_mini apps/redis_main.cpp)
target_link_libraries(redis_mini PRIVATE minidb_redis)
minidb_enable_warnings(redis_mini)

add_executable(neo4j_mini apps/neo4j_main.cpp)
target_link_libraries(neo4j_mini PRIVATE minidb_neo4j)
minidb_enable_warnings(neo4j_mini)

if(MINIDB_BUILD_TESTS)
    # 测试框架只使用标准库，避免教学项目被第三方测试依赖掩盖。
    include(CTest)
    add_executable(minidb_tests tests/minidb_tests.cpp)
    target_link_libraries(minidb_tests PRIVATE
        minidb_postgres
        minidb_mysql
        minidb_mongo
        minidb_redis
        minidb_neo4j)
    minidb_enable_warnings(minidb_tests)
    add_test(NAME minidb_tests COMMAND minidb_tests)
endif()
