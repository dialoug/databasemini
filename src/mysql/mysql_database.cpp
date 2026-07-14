#include "minidb/mysql/mysql_database.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace minidb::mysql {
namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

struct TableEntry {
    TableSchema schema;
    std::string engine;
};

QueryResult validate_row(const TableSchema& schema, const Row& row) {
    if (schema.columns.size() != row.size()) {
        return QueryResult::error("列数不匹配：期望 " + std::to_string(schema.columns.size()) +
                                  "，实际 " + std::to_string(row.size()));
    }
    for (std::size_t index = 0; index < row.size(); ++index) {
        const bool matches = (schema.columns[index].type == DataType::integer &&
                              std::holds_alternative<std::int64_t>(row[index])) ||
                             (schema.columns[index].type == DataType::text &&
                              std::holds_alternative<std::string>(row[index]));
        if (!matches) {
            return QueryResult::error("列 " + schema.columns[index].name + " 需要 " +
                                      data_type_name(schema.columns[index].type));
        }
    }
    return QueryResult::success();
}

std::vector<std::string> column_names(const TableSchema& schema) {
    std::vector<std::string> names;
    names.reserve(schema.columns.size());
    for (const auto& column : schema.columns) {
        names.push_back(column.name);
    }
    return names;
}

}  // namespace

struct MysqlDatabase::Impl {
    std::mutex mutex;
    std::uint64_t next_transaction_id{1};
    std::uint64_t next_connection_id{201};
    std::unordered_set<std::uint64_t> active_transactions;
    std::unordered_map<std::string, std::unique_ptr<StorageEngine>> engines;
    std::unordered_map<std::string, TableEntry> catalog;

    Impl() {
        engines.emplace("INNODB", make_innodb_engine());
        engines.emplace("MEMORY", make_memory_engine());
    }

    std::uint64_t begin_transaction() {
        const auto id = next_transaction_id++;
        active_transactions.insert(id);
        for (auto& [name, engine] : engines) {
            (void)name;
            engine->begin(id);
        }
        return id;
    }

    void commit_transaction(std::uint64_t id) {
        for (auto& [name, engine] : engines) {
            (void)name;
            engine->commit(id);
        }
        active_transactions.erase(id);
    }

    void rollback_transaction(std::uint64_t id) {
        for (auto& [name, engine] : engines) {
            (void)name;
            engine->rollback(id);
        }
        active_transactions.erase(id);
    }

    ReadView create_read_view(std::optional<std::uint64_t> self) const {
        ReadView view;
        view.low_limit_id = next_transaction_id;
        view.active_transactions = active_transactions;
        if (self) {
            view.active_transactions.erase(*self);
            view.self = self;
        }
        return view;
    }
};

MysqlDatabase::MysqlDatabase() : impl_(std::make_unique<Impl>()) {}
MysqlDatabase::~MysqlDatabase() = default;

std::unique_ptr<MysqlSession> MysqlDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    return std::unique_ptr<MysqlSession>(new MysqlSession(*this, impl_->next_connection_id++));
}

MysqlSession::MysqlSession(MysqlDatabase& database, std::uint64_t connection_id)
    : database_(&database), connection_id_(connection_id) {}

MysqlSession::~MysqlSession() {
    if (database_ != nullptr) {
        database_->rollback_on_disconnect(*this);
    }
}

QueryResult MysqlSession::execute(const std::string& sql) {
    const auto parsed = parse_sql(sql);
    if (!parsed.ok()) {
        return QueryResult::error(parsed.error);
    }
    return database_->execute(*this, *parsed.statement);
}

void MysqlDatabase::rollback_on_disconnect(MysqlSession& session) {
    std::lock_guard lock(impl_->mutex);
    if (session.transaction_id_) {
        impl_->rollback_transaction(*session.transaction_id_);
        session.transaction_id_.reset();
        session.consistent_read_view_.reset();
    }
}

QueryResult MysqlDatabase::execute(MysqlSession& session, const Statement& statement) {
    std::lock_guard lock(impl_->mutex);

    return std::visit(Overloaded{
        [&](const CreateTableStatement& create) -> QueryResult {
            auto schema = create.schema;
            schema.name = ascii_lower(schema.name);
            if (schema.columns.empty()) {
                return QueryResult::error("表至少需要一列");
            }
            std::unordered_set<std::string> seen_columns;
            for (auto& column : schema.columns) {
                column.name = ascii_lower(column.name);
                if (!seen_columns.insert(column.name).second) {
                    return QueryResult::error("重复列名: " + column.name);
                }
            }
            if (impl_->catalog.contains(schema.name)) {
                return QueryResult::error("表已存在: " + schema.name);
            }
            const auto engine_name = create.engine.empty() ? std::string("innodb")
                                                           : ascii_lower(create.engine);
            std::string normalized_engine;
            for (const auto& [name, engine] : impl_->engines) {
                (void)engine;
                if (ascii_lower(name) == engine_name) {
                    normalized_engine = name;
                    break;
                }
            }
            if (normalized_engine.empty()) {
                return QueryResult::error("未知存储引擎: " + create.engine);
            }

            bool implicit_commit = false;
            if (session.transaction_id_) {
                impl_->commit_transaction(*session.transaction_id_);
                session.transaction_id_.reset();
                session.consistent_read_view_.reset();
                implicit_commit = true;
            }
            auto result = impl_->engines.at(normalized_engine)->create_table(schema);
            if (!result.ok) {
                return result;
            }
            impl_->catalog.emplace(schema.name, TableEntry{schema, normalized_engine});
            return QueryResult::success(
                "CREATE TABLE; ENGINE=" + normalized_engine +
                (implicit_commit ? "（DDL 触发隐式 COMMIT）" : ""));
        },
        [&](const InsertStatement& insert) -> QueryResult {
            const auto name = ascii_lower(insert.table);
            const auto table_iterator = impl_->catalog.find(name);
            if (table_iterator == impl_->catalog.end()) {
                return QueryResult::error("表不存在: " + name);
            }
            auto validation = validate_row(table_iterator->second.schema, insert.values);
            if (!validation.ok) {
                return validation;
            }
            const bool autocommit = !session.transaction_id_;
            if (autocommit) {
                session.transaction_id_ = impl_->begin_transaction();
            }
            const auto transaction_id = *session.transaction_id_;
            auto result = impl_->engines.at(table_iterator->second.engine)
                              ->insert(name, insert.values, transaction_id);
            if (!result.ok) {
                if (autocommit) {
                    impl_->rollback_transaction(transaction_id);
                    session.transaction_id_.reset();
                }
                return result;
            }
            if (autocommit) {
                impl_->commit_transaction(transaction_id);
                session.transaction_id_.reset();
            }
            return QueryResult::success("Query OK, 1 row affected; ENGINE=" +
                                        table_iterator->second.engine);
        },
        [&](const SelectStatement& select) -> QueryResult {
            const auto name = ascii_lower(select.table);
            const auto table_iterator = impl_->catalog.find(name);
            if (table_iterator == impl_->catalog.end()) {
                return QueryResult::error("表不存在: " + name);
            }
            ReadView view;
            if (session.transaction_id_) {
                // InnoDB REPEATABLE READ: reuse the first consistent-read view.
                if (!session.consistent_read_view_) {
                    session.consistent_read_view_ = impl_->create_read_view(session.transaction_id_);
                }
                view = *session.consistent_read_view_;
            } else {
                view = impl_->create_read_view(std::nullopt);
            }
            auto rows = impl_->engines.at(table_iterator->second.engine)->select(name, view);
            return QueryResult::table(column_names(table_iterator->second.schema), std::move(rows),
                                      "SELECT; ENGINE=" + table_iterator->second.engine +
                                          (table_iterator->second.engine == "INNODB"
                                               ? "（事务级 consistent read view）"
                                               : "（非事务，立即可见）"));
        },
        [&](const BeginStatement&) -> QueryResult {
            if (session.transaction_id_) {
                return QueryResult::error("已经处于事务中");
            }
            session.transaction_id_ = impl_->begin_transaction();
            session.consistent_read_view_.reset();
            return QueryResult::success("START TRANSACTION; trx_id=" +
                                        std::to_string(*session.transaction_id_));
        },
        [&](const CommitStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                return QueryResult::success("COMMIT（当前没有事务）");
            }
            const auto id = *session.transaction_id_;
            impl_->commit_transaction(id);
            session.transaction_id_.reset();
            session.consistent_read_view_.reset();
            return QueryResult::success("COMMIT; trx_id=" + std::to_string(id));
        },
        [&](const RollbackStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                return QueryResult::success("ROLLBACK（当前没有事务）");
            }
            const auto id = *session.transaction_id_;
            impl_->rollback_transaction(id);
            session.transaction_id_.reset();
            session.consistent_read_view_.reset();
            return QueryResult::success(
                "ROLLBACK; trx_id=" + std::to_string(id) +
                "（InnoDB 撤销；MEMORY 写入保留）");
        },
        [&](const ShowArchitectureStatement&) -> QueryResult {
            return QueryResult::table(
                {"组件", "本项目中的体现", "真实 MySQL 对应物"},
                {
                    Row{std::string("mysqld"), std::string("MysqlDatabase"), std::string("单服务器进程")},
                    Row{std::string("connection thread"), std::string("MysqlSession id=") + std::to_string(session.connection_id_), std::string("连接/工作线程")},
                    Row{std::string("SQL layer"), std::string("parser + catalog + router"), std::string("Server SQL 层")},
                    Row{std::string("handler API"), std::string("StorageEngine interface"), std::string("可插拔存储引擎接口")},
                    Row{std::string("事务隔离"), std::string("first consistent read view"), std::string("InnoDB 默认 REPEATABLE READ")},
                },
                "单进程多连接线程模型（本项目用逻辑 connection 模拟）");
        },
        [&](const ShowEnginesStatement&) -> QueryResult {
            std::vector<Row> rows;
            for (const auto& [name, engine] : impl_->engines) {
                rows.push_back(Row{name,
                                   std::string(engine->transactional() ? "YES" : "NO"),
                                   engine->description()});
            }
            return QueryResult::table({"Engine", "Transactions", "Comment"}, std::move(rows),
                                      "2 storage engine(s)");
        },
        [&](const VacuumStatement&) -> QueryResult {
            return QueryResult::error("VACUUM 是 PostgreSQL 命令；MySQL 的维护由各存储引擎负责");
        },
        [&](const HelpStatement&) -> QueryResult {
            return QueryResult::success(
                "支持语法:\n"
                "  CREATE TABLE t (id INT, name TEXT) [ENGINE=INNODB|MEMORY];\n"
                "  INSERT INTO t VALUES (1, 'Alice');\n"
                "  SELECT * FROM t;\n"
                "  START TRANSACTION;  COMMIT;  ROLLBACK;\n"
                "  SHOW ENGINES;  SHOW ARCHITECTURE;\n"
                "  HELP;  QUIT;");
        },
        [&](const QuitStatement&) -> QueryResult {
            return QueryResult::success("QUIT");
        },
    }, statement);
}

}  // namespace minidb::mysql
