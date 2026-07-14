#include "minidb/postgres/postgres_database.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace minidb::postgres {
namespace {

template <class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

enum class TransactionStatus { in_progress, committed, aborted };

struct Snapshot {
    std::uint64_t xmax{};
    std::unordered_set<std::uint64_t> active_transactions;
};

struct TupleVersion {
    Row values;
    std::uint64_t xmin{};
};

struct HeapTable {
    TableSchema schema;
    std::uint64_t created_by{};
    std::vector<TupleVersion> versions;
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

struct PostgresDatabase::Impl {
    std::mutex mutex;
    std::uint64_t next_transaction_id{1};
    std::uint64_t next_backend_pid{10001};
    std::unordered_map<std::uint64_t, TransactionStatus> transaction_status;
    std::unordered_map<std::string, HeapTable> tables;

    std::uint64_t begin_transaction() {
        const auto id = next_transaction_id++;
        transaction_status[id] = TransactionStatus::in_progress;
        return id;
    }

    Snapshot statement_snapshot(std::optional<std::uint64_t> self) const {
        Snapshot snapshot;
        snapshot.xmax = next_transaction_id;
        for (const auto& [id, status] : transaction_status) {
            if (status == TransactionStatus::in_progress && (!self || id != *self)) {
                snapshot.active_transactions.insert(id);
            }
        }
        return snapshot;
    }

    bool visible(const TupleVersion& version,
                 const Snapshot& snapshot,
                 std::optional<std::uint64_t> self) const {
        if (self && version.xmin == *self) {
            return true;
        }
        const auto status = transaction_status.find(version.xmin);
        return status != transaction_status.end() &&
               status->second == TransactionStatus::committed &&
               version.xmin < snapshot.xmax &&
               !snapshot.active_transactions.contains(version.xmin);
    }

    bool table_visible(const HeapTable& table, std::optional<std::uint64_t> self) const {
        return table.created_by == 0 || (self && table.created_by == *self);
    }

    void commit_transaction(std::uint64_t id) {
        transaction_status[id] = TransactionStatus::committed;
        for (auto& [name, table] : tables) {
            (void)name;
            if (table.created_by == id) {
                table.created_by = 0;
            }
        }
    }

    void abort_transaction(std::uint64_t id) {
        transaction_status[id] = TransactionStatus::aborted;
        std::erase_if(tables, [id](const auto& item) { return item.second.created_by == id; });
    }
};

PostgresDatabase::PostgresDatabase() : impl_(std::make_unique<Impl>()) {}
PostgresDatabase::~PostgresDatabase() = default;

std::unique_ptr<PostgresSession> PostgresDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    return std::unique_ptr<PostgresSession>(new PostgresSession(*this, impl_->next_backend_pid++));
}

PostgresSession::PostgresSession(PostgresDatabase& database, std::uint64_t backend_pid)
    : database_(&database), backend_pid_(backend_pid) {}

PostgresSession::~PostgresSession() {
    if (database_ != nullptr) {
        database_->rollback_on_disconnect(*this);
    }
}

QueryResult PostgresSession::execute(const std::string& sql) {
    const auto parsed = parse_sql(sql);
    if (!parsed.ok()) {
        if (transaction_id_) {
            failed_transaction_ = true;
        }
        return QueryResult::error(parsed.error);
    }

    const bool is_rollback = std::holds_alternative<RollbackStatement>(*parsed.statement);
    if (failed_transaction_ && !is_rollback) {
        return QueryResult::error(
            "当前事务已中止，后续命令被忽略；请先执行 ROLLBACK（PostgreSQL 行为）");
    }

    auto result = database_->execute(*this, *parsed.statement);
    if (!result.ok && transaction_id_ && !is_rollback) {
        failed_transaction_ = true;
    }
    return result;
}

void PostgresDatabase::rollback_on_disconnect(PostgresSession& session) {
    std::lock_guard lock(impl_->mutex);
    if (session.transaction_id_) {
        impl_->abort_transaction(*session.transaction_id_);
        session.transaction_id_.reset();
        session.failed_transaction_ = false;
    }
}

QueryResult PostgresDatabase::execute(PostgresSession& session, const Statement& statement) {
    std::lock_guard lock(impl_->mutex);

    return std::visit(Overloaded{
        [&](const CreateTableStatement& create) -> QueryResult {
            if (!create.engine.empty()) {
                return QueryResult::error("PostgreSQL 没有 MySQL 式 ENGINE= 存储引擎子句");
            }
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
            if (impl_->tables.contains(schema.name)) {
                return QueryResult::error("表已存在: " + schema.name);
            }

            const bool autocommit = !session.transaction_id_;
            if (autocommit) {
                session.transaction_id_ = impl_->begin_transaction();
            }
            const auto xid = *session.transaction_id_;
            impl_->tables.emplace(schema.name, HeapTable{schema, xid, {}});
            if (autocommit) {
                impl_->commit_transaction(xid);
                session.transaction_id_.reset();
            }
            return QueryResult::success("CREATE TABLE（heap，DDL 可随事务回滚）");
        },
        [&](const InsertStatement& insert) -> QueryResult {
            const auto name = ascii_lower(insert.table);
            const auto table_iterator = impl_->tables.find(name);
            if (table_iterator == impl_->tables.end() ||
                !impl_->table_visible(table_iterator->second, session.transaction_id_)) {
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
            const auto xid = *session.transaction_id_;
            table_iterator->second.versions.push_back(TupleVersion{insert.values, xid});
            if (autocommit) {
                impl_->commit_transaction(xid);
                session.transaction_id_.reset();
            }
            return QueryResult::success("INSERT 0 1（追加一个 heap tuple version）");
        },
        [&](const SelectStatement& select) -> QueryResult {
            const auto name = ascii_lower(select.table);
            const auto table_iterator = impl_->tables.find(name);
            if (table_iterator == impl_->tables.end() ||
                !impl_->table_visible(table_iterator->second, session.transaction_id_)) {
                return QueryResult::error("表不存在: " + name);
            }
            // PostgreSQL READ COMMITTED: every statement gets a fresh snapshot.
            const auto snapshot = impl_->statement_snapshot(session.transaction_id_);
            std::vector<Row> rows;
            for (const auto& version : table_iterator->second.versions) {
                if (impl_->visible(version, snapshot, session.transaction_id_)) {
                    rows.push_back(version.values);
                }
            }
            return QueryResult::table(column_names(table_iterator->second.schema), std::move(rows),
                                      "SELECT（语句级 MVCC snapshot）");
        },
        [&](const BeginStatement&) -> QueryResult {
            if (session.transaction_id_) {
                return QueryResult::error("已经处于事务中");
            }
            session.transaction_id_ = impl_->begin_transaction();
            session.failed_transaction_ = false;
            return QueryResult::success("BEGIN; xid=" + std::to_string(*session.transaction_id_));
        },
        [&](const CommitStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                return QueryResult::success("COMMIT（当前没有事务）");
            }
            const auto xid = *session.transaction_id_;
            impl_->commit_transaction(xid);
            session.transaction_id_.reset();
            session.failed_transaction_ = false;
            return QueryResult::success("COMMIT; xid=" + std::to_string(xid));
        },
        [&](const RollbackStatement&) -> QueryResult {
            if (!session.transaction_id_) {
                session.failed_transaction_ = false;
                return QueryResult::success("ROLLBACK（当前没有事务）");
            }
            const auto xid = *session.transaction_id_;
            impl_->abort_transaction(xid);
            session.transaction_id_.reset();
            session.failed_transaction_ = false;
            return QueryResult::success("ROLLBACK; xid=" + std::to_string(xid));
        },
        [&](const ShowArchitectureStatement&) -> QueryResult {
            return QueryResult::table(
                {"组件", "本项目中的体现", "真实 PostgreSQL 对应物"},
                {
                    Row{std::string("postmaster"), std::string("PostgresDatabase"), std::string("监听与派生 backend")},
                    Row{std::string("backend process"), std::string("PostgresSession pid=") + std::to_string(session.backend_pid_), std::string("每连接独立进程")},
                    Row{std::string("heap + MVCC"), std::string("xmin tuple versions"), std::string("共享缓冲区中的 heap page")},
                    Row{std::string("事务隔离"), std::string("statement snapshot"), std::string("默认 READ COMMITTED")},
                    Row{std::string("vacuum"), std::string("删除 aborted versions"), std::string("回收 dead tuples")},
                },
                "逻辑进程模型（未创建真实 OS 子进程）");
        },
        [&](const ShowEnginesStatement&) -> QueryResult {
            return QueryResult::error("PostgreSQL 核心表访问方法不是 MySQL 式可插拔存储引擎；请执行 SHOW ARCHITECTURE");
        },
        [&](const VacuumStatement& vacuum) -> QueryResult {
            if (session.transaction_id_) {
                return QueryResult::error("VACUUM 不能在事务块内运行");
            }
            std::size_t removed = 0;
            const auto vacuum_table = [&](HeapTable& table) {
                const auto old_size = table.versions.size();
                std::erase_if(table.versions, [&](const TupleVersion& version) {
                    const auto status = impl_->transaction_status.find(version.xmin);
                    return status != impl_->transaction_status.end() &&
                           status->second == TransactionStatus::aborted;
                });
                removed += old_size - table.versions.size();
            };
            if (vacuum.table) {
                const auto name = ascii_lower(*vacuum.table);
                const auto iterator = impl_->tables.find(name);
                if (iterator == impl_->tables.end()) {
                    return QueryResult::error("表不存在: " + name);
                }
                vacuum_table(iterator->second);
            } else {
                for (auto& [name, table] : impl_->tables) {
                    (void)name;
                    vacuum_table(table);
                }
            }
            return QueryResult::success("VACUUM：回收 " + std::to_string(removed) + " 个 tuple version");
        },
        [&](const HelpStatement&) -> QueryResult {
            return QueryResult::success(
                "支持语法:\n"
                "  CREATE TABLE t (id INT, name TEXT);\n"
                "  INSERT INTO t VALUES (1, 'Alice');\n"
                "  SELECT * FROM t;\n"
                "  BEGIN;  COMMIT;  ROLLBACK;\n"
                "  VACUUM [t];\n"
                "  SHOW ARCHITECTURE;\n"
                "  HELP;  QUIT;");
        },
        [&](const QuitStatement&) -> QueryResult {
            return QueryResult::success("QUIT");
        },
    }, statement);
}

}  // namespace minidb::postgres
