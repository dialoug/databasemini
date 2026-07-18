#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace minidb::neo4j {

// Neo4j 属性图中的属性是标量值；本教学子集保留最常见的四种类型。
using PropertyValue = std::variant<std::nullptr_t, bool, std::int64_t, std::string>;
using PropertyMap = std::map<std::string, PropertyValue>;

struct NodePattern {
    std::optional<std::string> variable;
    std::vector<std::string> labels;
    PropertyMap properties;
};

struct RelationshipPattern {
    std::optional<std::string> variable;
    std::optional<std::string> type;
    PropertyMap properties;
};

struct GraphPattern {
    NodePattern left;
    std::optional<RelationshipPattern> relationship;
    std::optional<NodePattern> right;
};

struct ReturnItem {
    std::string variable;
    std::optional<std::string> property;

    [[nodiscard]] std::string display_name() const;
};

struct CreateStatement {
    GraphPattern pattern;
};

struct MatchStatement {
    GraphPattern pattern;
    std::vector<ReturnItem> returns;
};

struct BeginStatement {};
struct CommitStatement {};
struct RollbackStatement {};
struct ShowLabelsStatement {};
struct ShowRelationshipTypesStatement {};
struct ShowArchitectureStatement {};
struct HelpStatement {};
struct QuitStatement {};

using Statement = std::variant<CreateStatement,
                               MatchStatement,
                               BeginStatement,
                               CommitStatement,
                               RollbackStatement,
                               ShowLabelsStatement,
                               ShowRelationshipTypesStatement,
                               ShowArchitectureStatement,
                               HelpStatement,
                               QuitStatement>;

struct ParseResult {
    std::optional<Statement> statement;
    std::string error;

    [[nodiscard]] bool ok() const { return statement.has_value(); }
};

// 解析一行 Neo4j/Cypher 风格教学命令。MATCH 当前只支持一个节点或一条有向边。
ParseResult parse_cypher(const std::string& command);

}  // namespace minidb::neo4j
