#include "minidb/common/repl.hpp"
#include "minidb/mongo/mongo_database.hpp"

#include <string>

int main() {
    // 一个 Database 模拟 mongod，Session 模拟驱动创建的逻辑会话。
    minidb::mongo::MongoDatabase database;
    auto session = database.connect();
    const auto banner =
        "mongo_mini - MongoDB 架构教学数据库\n"
        "逻辑 mongod -> collection -> flexible BSON documents / snapshot transaction\n"
        "当前 session id: " + std::to_string(session->session_id());
    return minidb::run_repl(
        banner, "mongo_mini> ",
        [&](const std::string& command) { return session->execute(command); });
}
