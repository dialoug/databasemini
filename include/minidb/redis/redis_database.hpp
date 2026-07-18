#pragma once

#include "minidb/common/result.hpp"
#include "minidb/redis/redis_parser.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minidb::redis {

class RedisSession;

// RedisDatabase 表示一个逻辑 redis-server；教学 keyspace 只实现 string 类型。
class RedisDatabase {
public:
    RedisDatabase();
    ~RedisDatabase();

    RedisDatabase(const RedisDatabase&) = delete;
    RedisDatabase& operator=(const RedisDatabase&) = delete;

    std::unique_ptr<RedisSession> connect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(RedisSession& session, const Statement& statement);

    friend class RedisSession;
};

// MULTI 队列属于连接上下文；连接断开或 DISCARD 时尚未 EXEC 的命令直接丢弃。
class RedisSession {
public:
    ~RedisSession();

    RedisSession(const RedisSession&) = delete;
    RedisSession& operator=(const RedisSession&) = delete;

    QueryResult execute(const std::string& command);
    [[nodiscard]] std::uint64_t connection_id() const { return connection_id_; }
    [[nodiscard]] bool in_multi() const { return in_multi_; }
    [[nodiscard]] std::size_t queued_commands() const { return queued_commands_.size(); }

private:
    RedisSession(RedisDatabase& database, std::uint64_t connection_id);

    RedisDatabase* database_;
    std::uint64_t connection_id_;
    bool in_multi_{false};
    std::vector<Statement> queued_commands_;

    friend class RedisDatabase;
};

}  // namespace minidb::redis
