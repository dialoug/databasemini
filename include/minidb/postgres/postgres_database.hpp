#pragma once

#include "minidb/common/result.hpp"
#include "minidb/common/sql_parser.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace minidb::postgres {

class PostgresSession;

// PostgresDatabase 对应教学模型中的 postmaster 和共享数据库状态。
// 真实 PostgreSQL 会派生 OS backend 进程；这里用 Session 对象做可移植模拟。
class PostgresDatabase {
public:
    PostgresDatabase();
    ~PostgresDatabase();

    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;

    std::unique_ptr<PostgresSession> connect();

private:
    // PImpl 隐藏事务表、heap 和互斥锁，保持公开头文件易读且编译依赖较少。
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(PostgresSession& session, const Statement& statement);
    void rollback_on_disconnect(PostgresSession& session);

    friend class PostgresSession;
};

// 每个 PostgresSession 代表一个独立 backend，拥有自己的 xid 和事务失败状态。
class PostgresSession {
public:
    ~PostgresSession();

    PostgresSession(const PostgresSession&) = delete;
    PostgresSession& operator=(const PostgresSession&) = delete;

    QueryResult execute(const std::string& sql);
    [[nodiscard]] std::uint64_t backend_pid() const { return backend_pid_; }
    [[nodiscard]] bool in_transaction() const { return transaction_id_.has_value(); }

private:
    PostgresSession(PostgresDatabase& database, std::uint64_t backend_pid);

    PostgresDatabase* database_;
    std::uint64_t backend_pid_;
    std::optional<std::uint64_t> transaction_id_;
    // PostgreSQL 中事务内语句失败后，除 ROLLBACK 外的命令都会被拒绝。
    bool failed_transaction_{false};

    friend class PostgresDatabase;
};

}  // namespace minidb::postgres
