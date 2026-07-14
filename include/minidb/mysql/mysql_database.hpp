#pragma once

#include "minidb/common/result.hpp"
#include "minidb/common/sql_parser.hpp"
#include "minidb/mysql/storage_engine.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace minidb::mysql {

class MysqlSession;

class MysqlDatabase {
public:
    MysqlDatabase();
    ~MysqlDatabase();

    MysqlDatabase(const MysqlDatabase&) = delete;
    MysqlDatabase& operator=(const MysqlDatabase&) = delete;

    std::unique_ptr<MysqlSession> connect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(MysqlSession& session, const Statement& statement);
    void rollback_on_disconnect(MysqlSession& session);

    friend class MysqlSession;
};

class MysqlSession {
public:
    ~MysqlSession();

    MysqlSession(const MysqlSession&) = delete;
    MysqlSession& operator=(const MysqlSession&) = delete;

    QueryResult execute(const std::string& sql);
    [[nodiscard]] std::uint64_t connection_id() const { return connection_id_; }
    [[nodiscard]] bool in_transaction() const { return transaction_id_.has_value(); }

private:
    MysqlSession(MysqlDatabase& database, std::uint64_t connection_id);

    MysqlDatabase* database_;
    std::uint64_t connection_id_;
    std::optional<std::uint64_t> transaction_id_;
    std::optional<ReadView> consistent_read_view_;

    friend class MysqlDatabase;
};

}  // namespace minidb::mysql
