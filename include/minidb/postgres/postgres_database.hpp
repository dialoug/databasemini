#pragma once

#include "minidb/common/result.hpp"
#include "minidb/common/sql_parser.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace minidb::postgres {

class PostgresSession;

class PostgresDatabase {
public:
    PostgresDatabase();
    ~PostgresDatabase();

    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;

    std::unique_ptr<PostgresSession> connect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(PostgresSession& session, const Statement& statement);
    void rollback_on_disconnect(PostgresSession& session);

    friend class PostgresSession;
};

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
    bool failed_transaction_{false};

    friend class PostgresDatabase;
};

}  // namespace minidb::postgres
