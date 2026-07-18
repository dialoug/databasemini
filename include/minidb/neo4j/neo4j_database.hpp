#pragma once

#include "minidb/common/result.hpp"
#include "minidb/neo4j/cypher_parser.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace minidb::neo4j {

class Neo4jSession;

// Neo4jDatabase 表示一个逻辑图数据库服务，内部保存属性节点和有向关系。
class Neo4jDatabase {
public:
    Neo4jDatabase();
    ~Neo4jDatabase();

    Neo4jDatabase(const Neo4jDatabase&) = delete;
    Neo4jDatabase& operator=(const Neo4jDatabase&) = delete;

    std::unique_ptr<Neo4jSession> connect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QueryResult execute(Neo4jSession& session, const Statement& statement);
    void disconnect(Neo4jSession& session);

    friend class Neo4jSession;
};

// Session 模拟 Neo4j driver session；显式事务使用独立图快照并在提交时原子发布。
class Neo4jSession {
public:
    ~Neo4jSession();

    Neo4jSession(const Neo4jSession&) = delete;
    Neo4jSession& operator=(const Neo4jSession&) = delete;

    QueryResult execute(const std::string& cypher);
    [[nodiscard]] std::uint64_t session_id() const { return session_id_; }
    [[nodiscard]] bool in_transaction() const { return in_transaction_; }

private:
    Neo4jSession(Neo4jDatabase& database, std::uint64_t session_id);

    Neo4jDatabase* database_;
    std::uint64_t session_id_;
    bool in_transaction_{false};

    friend class Neo4jDatabase;
};

}  // namespace minidb::neo4j
