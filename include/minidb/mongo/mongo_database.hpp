#pragma once

#include "minidb/common/result.hpp"
#include "minidb/mongo/mongo_parser.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace minidb::mongo {

class MongoSession;

// MongoDatabase 表示一个逻辑 mongod：持有 collection catalog、文档版本和事务状态。
class MongoDatabase {
public:
    MongoDatabase();
    ~MongoDatabase();

    MongoDatabase(const MongoDatabase&) = delete;
    MongoDatabase& operator=(const MongoDatabase&) = delete;

    std::unique_ptr<MongoSession> connect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(MongoSession& session, const Statement& statement);
    void rollback_on_disconnect(MongoSession& session);

    friend class MongoSession;
};

// Session 模拟 MongoDB 驱动会话；事务内读取复用开始事务时的快照。
class MongoSession {
public:
    ~MongoSession();

    MongoSession(const MongoSession&) = delete;
    MongoSession& operator=(const MongoSession&) = delete;

    QueryResult execute(const std::string& command);
    [[nodiscard]] std::uint64_t session_id() const { return session_id_; }
    [[nodiscard]] bool in_transaction() const { return transaction_id_.has_value(); }

private:
    MongoSession(MongoDatabase& database, std::uint64_t session_id);

    MongoDatabase* database_;
    std::uint64_t session_id_;
    std::optional<std::uint64_t> transaction_id_;

    friend class MongoDatabase;
};

}  // namespace minidb::mongo
