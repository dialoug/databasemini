#include "minidb/neo4j/neo4j_database.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::neo4j {
namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

struct Node {
    std::int64_t id;
    std::vector<std::string> labels;
    PropertyMap properties;
};

struct Relationship {
    std::int64_t id;
    std::int64_t start_node_id;
    std::int64_t end_node_id;
    std::string type;
    PropertyMap properties;
};

struct GraphState {
    std::int64_t next_node_id{0};
    std::int64_t next_relationship_id{0};
    std::vector<Node> nodes;
    std::vector<Relationship> relationships;
};

struct TransactionState {
    GraphState graph;
    std::uint64_t base_revision;
    bool dirty{false};
};

std::string escape_string(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (const auto character : input) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '\'': result += "\\'"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(character); break;
        }
    }
    return result;
}

std::string property_to_string(const PropertyValue& property) {
    return std::visit(Overloaded{
        [](std::nullptr_t) { return std::string("null"); },
        [](bool value) { return std::string(value ? "true" : "false"); },
        [](std::int64_t value) { return std::to_string(value); },
        [](const std::string& value) { return "'" + escape_string(value) + "'"; },
    }, property);
}

Cell property_to_cell(const PropertyValue& property) {
    if (const auto integer = std::get_if<std::int64_t>(&property)) {
        return *integer;
    }
    if (const auto text = std::get_if<std::string>(&property)) {
        return *text;
    }
    if (const auto boolean = std::get_if<bool>(&property)) {
        return std::string(*boolean ? "true" : "false");
    }
    return std::string("null");
}

std::string properties_to_string(const PropertyMap& properties) {
    if (properties.empty()) {
        return {};
    }
    std::ostringstream output;
    output << " {";
    bool first = true;
    for (const auto& [key, value] : properties) {
        if (!first) {
            output << ", ";
        }
        first = false;
        output << key << ": " << property_to_string(value);
    }
    output << '}';
    return output.str();
}

std::string node_to_string(const Node& node) {
    std::ostringstream output;
    output << "(id=" << node.id;
    for (const auto& label : node.labels) {
        output << ':' << label;
    }
    output << properties_to_string(node.properties) << ')';
    return output.str();
}

std::string relationship_to_string(const Relationship& relationship) {
    std::ostringstream output;
    output << "[id=" << relationship.id << ':' << relationship.type
           << properties_to_string(relationship.properties) << ']';
    return output.str();
}

bool properties_match(const PropertyMap& values, const PropertyMap& pattern) {
    return std::all_of(pattern.begin(), pattern.end(), [&](const auto& entry) {
        const auto found = values.find(entry.first);
        return found != values.end() && found->second == entry.second;
    });
}

bool node_matches(const Node& node, const NodePattern& pattern) {
    const auto has_all_labels = std::all_of(
        pattern.labels.begin(), pattern.labels.end(), [&](const auto& label) {
            return std::find(node.labels.begin(), node.labels.end(), label) !=
                   node.labels.end();
        });
    return has_all_labels && properties_match(node.properties, pattern.properties);
}

bool relationship_matches(const Relationship& relationship,
                          const RelationshipPattern& pattern) {
    return (!pattern.type.has_value() || relationship.type == *pattern.type) &&
           properties_match(relationship.properties, pattern.properties);
}

const Node* find_node(const GraphState& graph, std::int64_t id) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                    [&](const Node& node) { return node.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
}

Node& create_node(GraphState& graph, const NodePattern& pattern) {
    graph.nodes.push_back(
        Node{graph.next_node_id++, pattern.labels, pattern.properties});
    return graph.nodes.back();
}

QueryResult execute_create(GraphState& graph, const CreateStatement& create) {
    const auto left_id = create_node(graph, create.pattern.left).id;
    if (!create.pattern.relationship.has_value()) {
        return QueryResult::success("Created 1 node (id=" +
                                    std::to_string(left_id) + ")");
    }

    const auto right_id = create_node(graph, *create.pattern.right).id;
    const auto& relationship_pattern = *create.pattern.relationship;
    const auto relationship_id = graph.next_relationship_id++;
    graph.relationships.push_back(Relationship{
        relationship_id,
        left_id,
        right_id,
        *relationship_pattern.type,
        relationship_pattern.properties,
    });
    return QueryResult::success(
        "Created 2 nodes and 1 relationship (relationship id=" +
        std::to_string(relationship_id) + ")");
}

Cell return_node_item(const Node& node, const ReturnItem& item) {
    if (!item.property.has_value()) {
        return node_to_string(node);
    }
    const auto property = node.properties.find(*item.property);
    return property == node.properties.end() ? Cell{std::string("null")}
                                              : property_to_cell(property->second);
}

Cell return_relationship_item(const Relationship& relationship,
                              const ReturnItem& item) {
    if (!item.property.has_value()) {
        return relationship_to_string(relationship);
    }
    const auto property = relationship.properties.find(*item.property);
    return property == relationship.properties.end() ? Cell{std::string("null")}
                                                       : property_to_cell(property->second);
}

Row make_match_row(const MatchStatement& match,
                   const Node& left,
                   const Relationship* relationship,
                   const Node* right) {
    Row row;
    row.reserve(match.returns.size());
    for (const auto& item : match.returns) {
        if (match.pattern.left.variable.has_value() &&
            item.variable == *match.pattern.left.variable) {
            row.push_back(return_node_item(left, item));
        } else if (relationship != nullptr &&
                   match.pattern.relationship->variable.has_value() &&
                   item.variable == *match.pattern.relationship->variable) {
            row.push_back(return_relationship_item(*relationship, item));
        } else if (right != nullptr && match.pattern.right->variable.has_value() &&
                   item.variable == *match.pattern.right->variable) {
            row.push_back(return_node_item(*right, item));
        } else {
            row.push_back(std::string("null"));
        }
    }
    return row;
}

QueryResult execute_match(const GraphState& graph, const MatchStatement& match) {
    std::vector<std::string> columns;
    columns.reserve(match.returns.size());
    for (const auto& item : match.returns) {
        columns.push_back(item.display_name());
    }

    std::vector<Row> rows;
    if (!match.pattern.relationship.has_value()) {
        for (const auto& node : graph.nodes) {
            if (node_matches(node, match.pattern.left)) {
                rows.push_back(make_match_row(match, node, nullptr, nullptr));
            }
        }
        return QueryResult::table(std::move(columns), std::move(rows), "MATCH");
    }

    for (const auto& relationship : graph.relationships) {
        if (!relationship_matches(relationship, *match.pattern.relationship)) {
            continue;
        }
        const auto* left = find_node(graph, relationship.start_node_id);
        const auto* right = find_node(graph, relationship.end_node_id);
        if (left != nullptr && right != nullptr &&
            node_matches(*left, match.pattern.left) &&
            node_matches(*right, *match.pattern.right)) {
            rows.push_back(make_match_row(match, *left, &relationship, right));
        }
    }
    return QueryResult::table(std::move(columns), std::move(rows), "MATCH");
}

QueryResult show_labels(const GraphState& graph) {
    std::map<std::string, std::int64_t> counts;
    for (const auto& node : graph.nodes) {
        for (const auto& label : node.labels) {
            ++counts[label];
        }
    }
    std::vector<Row> rows;
    rows.reserve(counts.size());
    for (const auto& [label, count] : counts) {
        rows.push_back(Row{label, count});
    }
    return QueryResult::table({"label", "node_count"}, std::move(rows),
                              "SHOW LABELS");
}

QueryResult show_relationship_types(const GraphState& graph) {
    std::map<std::string, std::int64_t> counts;
    for (const auto& relationship : graph.relationships) {
        ++counts[relationship.type];
    }
    std::vector<Row> rows;
    rows.reserve(counts.size());
    for (const auto& [type, count] : counts) {
        rows.push_back(Row{type, count});
    }
    return QueryResult::table({"relationship_type", "count"}, std::move(rows),
                              "SHOW RELATIONSHIP TYPES");
}

}  // namespace

struct Neo4jDatabase::Impl {
    std::mutex mutex;
    std::uint64_t next_session_id{50001};
    std::uint64_t revision{0};
    GraphState committed;
    std::unordered_map<std::uint64_t, TransactionState> transactions;
};

Neo4jDatabase::Neo4jDatabase() : impl_(std::make_unique<Impl>()) {}
Neo4jDatabase::~Neo4jDatabase() = default;

std::unique_ptr<Neo4jSession> Neo4jDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    return std::unique_ptr<Neo4jSession>(
        new Neo4jSession(*this, impl_->next_session_id++));
}

Neo4jSession::Neo4jSession(Neo4jDatabase& database, std::uint64_t session_id)
    : database_(&database), session_id_(session_id) {}

Neo4jSession::~Neo4jSession() {
    if (database_ != nullptr) {
        database_->disconnect(*this);
    }
}

QueryResult Neo4jSession::execute(const std::string& cypher) {
    const auto parsed = parse_cypher(cypher);
    if (!parsed.ok()) {
        return QueryResult::error(parsed.error);
    }
    return database_->execute(*this, *parsed.statement);
}

void Neo4jDatabase::disconnect(Neo4jSession& session) {
    std::lock_guard lock(impl_->mutex);
    impl_->transactions.erase(session.session_id_);
    session.in_transaction_ = false;
    session.database_ = nullptr;
}

QueryResult Neo4jDatabase::execute(Neo4jSession& session, const Statement& statement) {
    std::lock_guard lock(impl_->mutex);

    if (std::holds_alternative<BeginStatement>(statement)) {
        if (session.in_transaction_) {
            return QueryResult::error("transaction already open");
        }
        impl_->transactions.emplace(
            session.session_id_,
            TransactionState{impl_->committed, impl_->revision, false});
        session.in_transaction_ = true;
        return QueryResult::success("BEGIN");
    }

    if (std::holds_alternative<CommitStatement>(statement)) {
        if (!session.in_transaction_) {
            return QueryResult::error("COMMIT without BEGIN");
        }
        auto transaction = impl_->transactions.find(session.session_id_);
        if (transaction == impl_->transactions.end()) {
            session.in_transaction_ = false;
            return QueryResult::error("transaction state is missing");
        }
        if (transaction->second.dirty &&
            transaction->second.base_revision != impl_->revision) {
            impl_->transactions.erase(transaction);
            session.in_transaction_ = false;
            return QueryResult::error(
                "concurrent graph update detected; transaction rolled back");
        }
        if (transaction->second.dirty) {
            impl_->committed = std::move(transaction->second.graph);
            ++impl_->revision;
        }
        impl_->transactions.erase(transaction);
        session.in_transaction_ = false;
        return QueryResult::success("COMMIT");
    }

    if (std::holds_alternative<RollbackStatement>(statement)) {
        if (!session.in_transaction_) {
            return QueryResult::error("ROLLBACK without BEGIN");
        }
        impl_->transactions.erase(session.session_id_);
        session.in_transaction_ = false;
        return QueryResult::success("ROLLBACK");
    }

    auto transaction = impl_->transactions.find(session.session_id_);
    GraphState& graph = transaction == impl_->transactions.end()
                            ? impl_->committed
                            : transaction->second.graph;

    return std::visit(Overloaded{
        [&](const CreateStatement& create) -> QueryResult {
            auto result = execute_create(graph, create);
            if (transaction == impl_->transactions.end()) {
                ++impl_->revision;
            } else {
                transaction->second.dirty = true;
            }
            return result;
        },
        [&](const MatchStatement& match) -> QueryResult {
            return execute_match(graph, match);
        },
        [&](const ShowLabelsStatement&) -> QueryResult {
            return show_labels(graph);
        },
        [&](const ShowRelationshipTypesStatement&) -> QueryResult {
            return show_relationship_types(graph);
        },
        [&](const ShowArchitectureStatement&) -> QueryResult {
            return QueryResult::table(
                {"组件", "本项目中的体现", "真实 Neo4j 对应物"},
                {
                    Row{std::string("graph server"), std::string("Neo4jDatabase"),
                        std::string("Neo4j DBMS")},
                    Row{std::string("driver session"),
                        std::string("Neo4jSession id=") +
                            std::to_string(session.session_id_),
                        std::string("Driver Session")},
                    Row{std::string("property graph"),
                        std::to_string(graph.nodes.size()) + " nodes / " +
                            std::to_string(graph.relationships.size()) + " relationships",
                        std::string("带标签节点与有类型关系")},
                    Row{std::string("query"), std::string("CREATE / one-hop MATCH"),
                        std::string("Cypher pattern matching")},
                    Row{std::string("transaction"),
                        session.in_transaction_ ? std::string("snapshot open")
                                                : std::string("auto-commit"),
                        std::string("ACID transaction")},
                },
                "Neo4j 属性图模型：关系是一等数据，可沿边直接遍历");
        },
        [](const HelpStatement&) -> QueryResult {
            return QueryResult::success(
                "支持语法:\n"
                "  CREATE (:Person {name: 'Alice'});\n"
                "  CREATE (a:Person)-[:KNOWS {since: 2020}]->(b:Person);\n"
                "  MATCH (n:Person {name: 'Alice'}) RETURN n, n.name;\n"
                "  MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a, r, b;\n"
                "  BEGIN;  COMMIT;  ROLLBACK;\n"
                "  SHOW LABELS;  SHOW RELATIONSHIP TYPES;\n"
                "  SHOW ARCHITECTURE;  HELP;  QUIT;\n"
                "范围限制：MATCH 只支持一个节点或一条从左到右的关系。");
        },
        [](const QuitStatement&) -> QueryResult {
            return QueryResult::success("QUIT");
        },
        [](const auto&) -> QueryResult {
            return QueryResult::error("内部错误：未处理的 Neo4j 命令");
        },
    }, statement);
}

}  // namespace minidb::neo4j
