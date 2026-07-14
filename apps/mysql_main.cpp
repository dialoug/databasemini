#include "minidb/common/repl.hpp"
#include "minidb/mysql/mysql_database.hpp"

#include <string>

int main() {
    // 一个 Database 表示一个 mysqld，connect() 创建独立的连接上下文。
    minidb::mysql::MysqlDatabase database;
    auto session = database.connect();
    const auto banner =
        "mysql_mini - MySQL 架构教学数据库\n"
        "逻辑 mysqld -> connection thread -> SQL layer -> pluggable engine\n"
        "当前 connection id: " + std::to_string(session->connection_id());
    // 通用 REPL 不知道存储引擎细节，只把 SQL 交给当前 MySQL Session。
    return minidb::run_repl(banner, "mysql_mini> ",
                            [&](const std::string& sql) { return session->execute(sql); });
}
