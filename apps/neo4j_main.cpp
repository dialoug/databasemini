#include "minidb/common/repl.hpp"
#include "minidb/neo4j/neo4j_database.hpp"

#include <string>

int main() {
    // 一个 Database 模拟 Neo4j server，Session 模拟 driver 的逻辑会话。
    minidb::neo4j::Neo4jDatabase database;
    auto session = database.connect();
    const auto banner =
        "neo4j_mini - Neo4j 属性图教学数据库\n"
        "逻辑 server -> labeled nodes -> typed relationships / Cypher traversal\n"
        "当前 session id: " + std::to_string(session->session_id());
    return minidb::run_repl(
        banner, "neo4j_mini> ",
        [&](const std::string& command) { return session->execute(command); });
}
