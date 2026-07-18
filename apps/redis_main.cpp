#include "minidb/common/repl.hpp"
#include "minidb/redis/redis_database.hpp"

#include <string>

int main() {
    // RedisDatabase 模拟 redis-server，Session 持有连接独立的 MULTI 命令队列。
    minidb::redis::RedisDatabase database;
    auto session = database.connect();
    const auto banner =
        "redis_mini - Redis 架构教学模型\n"
        "逻辑 redis-server -> connection -> string keyspace / MULTI command queue\n"
        "当前 connection id: " + std::to_string(session->connection_id());
    return minidb::run_repl(
        banner, "redis_mini> ",
        [&](const std::string& command) { return session->execute(command); });
}
