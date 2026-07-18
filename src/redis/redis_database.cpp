#include "minidb/redis/redis_database.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::redis {
namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

bool queueable(const Statement& statement) {
    return std::holds_alternative<SetStatement>(statement) ||
           std::holds_alternative<GetStatement>(statement) ||
           std::holds_alternative<DeleteStatement>(statement) ||
           std::holds_alternative<ExistsStatement>(statement) ||
           std::holds_alternative<IncrementStatement>(statement) ||
           std::holds_alternative<KeysStatement>(statement) ||
           std::holds_alternative<PingStatement>(statement) ||
           std::holds_alternative<TypeStatement>(statement) ||
           std::holds_alternative<DbSizeStatement>(statement);
}

std::string summarize_result(const QueryResult& result) {
    if (!result.ok) {
        return "ERROR: " + result.message;
    }
    if (!result.rows.empty()) {
        const auto& row = result.rows.front();
        if (row.size() >= 2) {
            return cell_to_string(row[1]);
        }
        if (!row.empty()) {
            return cell_to_string(row.front());
        }
    }
    return result.message.empty() ? "OK" : result.message;
}

bool pattern_supported(const std::string& pattern) {
    const auto wildcard = pattern.find('*');
    return wildcard == std::string::npos ||
           (wildcard == pattern.size() - 1 &&
            pattern.find('*', wildcard + 1) == std::string::npos);
}

bool key_matches(const std::string& key, const std::string& pattern) {
    const auto wildcard = pattern.find('*');
    return wildcard == std::string::npos ? key == pattern
                                         : key.starts_with(pattern.substr(0, wildcard));
}

}  // namespace

struct RedisDatabase::Impl {
    std::mutex mutex;
    std::uint64_t next_connection_id{40001};
    std::unordered_map<std::string, std::string> values;

    // 调用者必须持有 mutex。MULTI/EXEC 会只获取一次锁，再顺序调用此函数。
    QueryResult execute_data(const Statement& statement) {
        return std::visit(Overloaded{
            [&](const SetStatement& set) -> QueryResult {
                values[set.key] = set.value;
                return QueryResult::success("OK");
            },
            [&](const GetStatement& get) -> QueryResult {
                const auto value = values.find(get.key);
                if (value == values.end()) {
                    return QueryResult::success("(nil)");
                }
                return QueryResult::table(
                    {"key", "value"}, {Row{get.key, value->second}}, "GET");
            },
            [&](const DeleteStatement& remove) -> QueryResult {
                const auto erased = values.erase(remove.key);
                return QueryResult::success("(integer) " + std::to_string(erased));
            },
            [&](const ExistsStatement& exists) -> QueryResult {
                return QueryResult::success(
                    std::string("(integer) ") + (values.contains(exists.key) ? "1" : "0"));
            },
            [&](const IncrementStatement& increment) -> QueryResult {
                std::int64_t number = 0;
                const auto current = values.find(increment.key);
                if (current != values.end()) {
                    const auto& text = current->second;
                    const auto parsed =
                        std::from_chars(text.data(), text.data() + text.size(), number);
                    if (text.empty() || parsed.ec != std::errc{} ||
                        parsed.ptr != text.data() + text.size()) {
                        return QueryResult::error(
                            "value is not an integer or out of range: " + text);
                    }
                }
                if (number == std::numeric_limits<std::int64_t>::max()) {
                    return QueryResult::error("increment would overflow int64");
                }
                ++number;
                values[increment.key] = std::to_string(number);
                return QueryResult::success("(integer) " + std::to_string(number));
            },
            [&](const KeysStatement& keys) -> QueryResult {
                if (!pattern_supported(keys.pattern)) {
                    return QueryResult::error(
                        "教学版 KEYS 只支持精确 key 或末尾通配符，例如 user:*");
                }
                std::vector<std::string> matches;
                for (const auto& [key, value] : values) {
                    (void)value;
                    if (key_matches(key, keys.pattern)) {
                        matches.push_back(key);
                    }
                }
                std::sort(matches.begin(), matches.end());
                std::vector<Row> rows;
                rows.reserve(matches.size());
                for (auto& key : matches) {
                    rows.push_back(Row{std::move(key)});
                }
                return QueryResult::table({"key"}, std::move(rows), "KEYS");
            },
            [](const PingStatement& ping) -> QueryResult {
                return QueryResult::success(ping.message.value_or("PONG"));
            },
            [&](const TypeStatement& type) -> QueryResult {
                return QueryResult::success(values.contains(type.key) ? "string" : "none");
            },
            [&](const DbSizeStatement&) -> QueryResult {
                return QueryResult::success("(integer) " +
                                            std::to_string(values.size()));
            },
            [](const auto&) -> QueryResult {
                return QueryResult::error("内部错误：控制命令不能作为数据命令执行");
            },
        }, statement);
    }
};

RedisDatabase::RedisDatabase() : impl_(std::make_unique<Impl>()) {}
RedisDatabase::~RedisDatabase() = default;

std::unique_ptr<RedisSession> RedisDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    return std::unique_ptr<RedisSession>(
        new RedisSession(*this, impl_->next_connection_id++));
}

RedisSession::RedisSession(RedisDatabase& database, std::uint64_t connection_id)
    : database_(&database), connection_id_(connection_id) {}

RedisSession::~RedisSession() = default;

QueryResult RedisSession::execute(const std::string& command) {
    const auto parsed = parse_command(command);
    if (!parsed.ok()) {
        return QueryResult::error(parsed.error);
    }
    return database_->execute(*this, *parsed.statement);
}

QueryResult RedisDatabase::execute(RedisSession& session, const Statement& statement) {
    if (std::holds_alternative<MultiStatement>(statement)) {
        if (session.in_multi_) {
            return QueryResult::error("MULTI calls can not be nested");
        }
        session.in_multi_ = true;
        session.queued_commands_.clear();
        return QueryResult::success("OK");
    }

    if (std::holds_alternative<DiscardStatement>(statement)) {
        if (!session.in_multi_) {
            return QueryResult::error("DISCARD without MULTI");
        }
        const auto discarded = session.queued_commands_.size();
        session.queued_commands_.clear();
        session.in_multi_ = false;
        return QueryResult::success("OK; discarded " + std::to_string(discarded) +
                                    " queued command(s)");
    }

    if (std::holds_alternative<ExecStatement>(statement)) {
        if (!session.in_multi_) {
            return QueryResult::error("EXEC without MULTI");
        }
        auto commands = std::move(session.queued_commands_);
        session.queued_commands_.clear();
        session.in_multi_ = false;

        // 一次持锁覆盖整个队列，保证其他连接不会观察到 EXEC 的中间状态。
        std::lock_guard lock(impl_->mutex);
        std::vector<Row> rows;
        rows.reserve(commands.size());
        for (std::size_t index = 0; index < commands.size(); ++index) {
            auto result = impl_->execute_data(commands[index]);
            rows.push_back(Row{static_cast<std::int64_t>(index + 1),
                               summarize_result(result)});
        }
        return QueryResult::table(
            {"index", "result"}, std::move(rows),
            "EXEC：队列原子执行；单条运行时错误不会回滚其他命令");
    }

    if (queueable(statement)) {
        if (session.in_multi_) {
            session.queued_commands_.push_back(statement);
            return QueryResult::success("QUEUED");
        }
        std::lock_guard lock(impl_->mutex);
        return impl_->execute_data(statement);
    }

    return std::visit(Overloaded{
        [&](const ShowArchitectureStatement&) -> QueryResult {
            std::lock_guard lock(impl_->mutex);
            return QueryResult::table(
                {"组件", "本项目中的体现", "真实 Redis 对应物"},
                {
                    Row{std::string("redis-server"), std::string("RedisDatabase"),
                        std::string("Redis 服务进程")},
                    Row{std::string("connection"),
                        std::string("RedisSession id=") +
                            std::to_string(session.connection_id_),
                        std::string("客户端连接上下文")},
                    Row{std::string("keyspace"),
                        std::string("unordered_map<string, string>"),
                        std::string("key -> value 直接映射")},
                    Row{std::string("lookup"), std::string("hash lookup"),
                        std::string("无需 SQL planner 或文档过滤")},
                    Row{std::string("transaction"),
                        std::string("session queue + atomic EXEC"),
                        std::string("MULTI/EXEC/DISCARD")},
                },
                "Redis string 模型：直接 key 访问与服务端原子命令");
        },
        [](const HelpStatement&) -> QueryResult {
            return QueryResult::success(
                "支持语法:\n"
                "  PING [message];  TYPE key;  DBSIZE;\n"
                "  SET key value;  GET key;  DEL key;  EXISTS key;\n"
                "  INCR counter;  KEYS *;  KEYS user:*;\n"
                "  MULTI;  EXEC;  DISCARD;\n"
                "  SHOW ARCHITECTURE;  HELP;  QUIT;\n"
                "参数包含空格时使用单引号或双引号。");
        },
        [](const QuitStatement&) -> QueryResult {
            return QueryResult::success("QUIT");
        },
        [](const auto&) -> QueryResult {
            return QueryResult::error("内部错误：未处理的 Redis 命令");
        },
    }, statement);
}

}  // namespace minidb::redis
