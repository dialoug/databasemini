#pragma once

#include <optional>
#include <string>
#include <variant>

namespace minidb::redis {

struct SetStatement {
    std::string key;
    std::string value;
};

struct GetStatement {
    std::string key;
};

struct DeleteStatement {
    std::string key;
};

struct ExistsStatement {
    std::string key;
};

struct IncrementStatement {
    std::string key;
};

struct KeysStatement {
    std::string pattern;
};

struct PingStatement {
    std::optional<std::string> message;
};

struct TypeStatement {
    std::string key;
};

struct DbSizeStatement {};

struct MultiStatement {};
struct ExecStatement {};
struct DiscardStatement {};
struct ShowArchitectureStatement {};
struct HelpStatement {};
struct QuitStatement {};

using Statement = std::variant<SetStatement,
                               GetStatement,
                               DeleteStatement,
                               ExistsStatement,
                               IncrementStatement,
                               KeysStatement,
                               PingStatement,
                               TypeStatement,
                               DbSizeStatement,
                               MultiStatement,
                               ExecStatement,
                               DiscardStatement,
                               ShowArchitectureStatement,
                               HelpStatement,
                               QuitStatement>;

struct ParseResult {
    std::optional<Statement> statement;
    std::string error;

    [[nodiscard]] bool ok() const { return statement.has_value(); }
};

// 解析一行 Redis 风格教学命令；参数可直接书写，也可用单双引号包含空格。
ParseResult parse_command(const std::string& command);

}  // namespace minidb::redis
