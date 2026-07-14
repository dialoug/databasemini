#include "minidb/common/repl.hpp"
#include "minidb/postgres/postgres_database.hpp"

#include <string>

int main() {
    // Database 生命周期覆盖 Session，确保 backend 析构时仍可自动回滚未完成事务。
    minidb::postgres::PostgresDatabase database;
    auto session = database.connect();
    const auto banner =
        "pg_mini - PostgreSQL 架构教学数据库\n"
        "逻辑 postmaster -> 独立 backend session -> heap/MVCC（默认 READ COMMITTED）\n"
        "当前 backend pid: " + std::to_string(session->backend_pid());
    // lambda 把通用 REPL 的字符串输入转发给 PostgreSQL 风格执行器。
    return minidb::run_repl(banner, "pg_mini=> ",
                            [&](const std::string& sql) { return session->execute(sql); });
}
