#pragma once

#include "minidb/mongo/bson.hpp"

#include <optional>
#include <string>
#include <variant>

namespace minidb::mongo {

struct CreateCollectionStatement {
    std::string collection;
};

struct InsertOneStatement {
    std::string collection;
    Document document;
};

struct FindStatement {
    std::string collection;
    Document filter;
};

struct BeginStatement {};
struct CommitStatement {};
struct RollbackStatement {};
struct ShowCollectionsStatement {};
struct ShowArchitectureStatement {};
struct HelpStatement {};
struct QuitStatement {};

using Statement = std::variant<CreateCollectionStatement,
                               InsertOneStatement,
                               FindStatement,
                               BeginStatement,
                               CommitStatement,
                               RollbackStatement,
                               ShowCollectionsStatement,
                               ShowArchitectureStatement,
                               HelpStatement,
                               QuitStatement>;

struct ParseResult {
    std::optional<Statement> statement;
    std::string error;

    [[nodiscard]] bool ok() const { return statement.has_value(); }
};

// 解析一行 Mongo shell 风格教学命令。事务同时接受 BEGIN/COMMIT/ROLLBACK，
// 以便直接与本项目的 PostgreSQL、MySQL 示例对照。
ParseResult parse_command(const std::string& command);

}  // namespace minidb::mongo
