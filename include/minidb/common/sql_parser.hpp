#pragma once

#include "minidb/common/types.hpp"

#include <optional>
#include <string>
#include <variant>

namespace minidb {

struct CreateTableStatement {
    TableSchema schema;
    std::string engine;
};

struct InsertStatement {
    std::string table;
    Row values;
};

struct SelectStatement {
    std::string table;
};

struct BeginStatement {};
struct CommitStatement {};
struct RollbackStatement {};
struct ShowArchitectureStatement {};
struct ShowEnginesStatement {};
struct VacuumStatement {
    std::optional<std::string> table;
};
struct HelpStatement {};
struct QuitStatement {};

using Statement = std::variant<CreateTableStatement,
                               InsertStatement,
                               SelectStatement,
                               BeginStatement,
                               CommitStatement,
                               RollbackStatement,
                               ShowArchitectureStatement,
                               ShowEnginesStatement,
                               VacuumStatement,
                               HelpStatement,
                               QuitStatement>;

struct ParseResult {
    std::optional<Statement> statement;
    std::string error;

    [[nodiscard]] bool ok() const { return statement.has_value(); }
};

ParseResult parse_sql(const std::string& sql);

}  // namespace minidb
