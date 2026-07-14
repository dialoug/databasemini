#pragma once

#include "minidb/common/types.hpp"

#include <optional>
#include <string>
#include <variant>

namespace minidb {

// 以下结构是最小 SQL 抽象语法树（AST）。解析器只负责语法，不执行语句。
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

// 无参数语句使用空标记结构表示，variant 保证执行器必须处理每一种语句。
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

// 将一行 SQL 转成 AST；失败时 statement 为空，error 保存可读错误信息。
ParseResult parse_sql(const std::string& sql);

}  // namespace minidb
