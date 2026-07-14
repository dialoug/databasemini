#include "minidb/common/repl.hpp"
#include "minidb/mysql/mysql_database.hpp"

#include <string>

int main() {
    minidb::mysql::MysqlDatabase database;
    auto session = database.connect();
    const auto banner =
        "mysql_mini - MySQL 架构教学数据库\n"
        "逻辑 mysqld -> connection thread -> SQL layer -> pluggable engine\n"
        "当前 connection id: " + std::to_string(session->connection_id());
    return minidb::run_repl(banner, "mysql_mini> ",
                            [&](const std::string& sql) { return session->execute(sql); });
}
