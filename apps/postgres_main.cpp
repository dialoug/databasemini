#include "minidb/common/repl.hpp"
#include "minidb/postgres/postgres_database.hpp"

#include <string>

int main() {
    minidb::postgres::PostgresDatabase database;
    auto session = database.connect();
    const auto banner =
        "pg_mini - PostgreSQL 架构教学数据库\n"
        "逻辑 postmaster -> 独立 backend session -> heap/MVCC（默认 READ COMMITTED）\n"
        "当前 backend pid: " + std::to_string(session->backend_pid());
    return minidb::run_repl(banner, "pg_mini=> ",
                            [&](const std::string& sql) { return session->execute(sql); });
}
