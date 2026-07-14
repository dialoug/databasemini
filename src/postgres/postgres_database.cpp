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

// 事务状态表模拟 PostgreSQL 的提交状态信息；aborted 版本仍可能留在 heap 中。
enum class TransactionStatus { in_progress, committed, aborted };

// 语句快照记录创建时尚未完成的事务，以及当时尚未分配的 xid 下界。
struct Snapshot {
    std::uint64_t xmax{};
    std::unordered_set<std::uint64_t> active_transactions;
};

// heap 采用追加式版本：xmin 表示创建该 tuple 的事务。
// 当前 SQL 子集没有 UPDATE/DELETE，所以暂时不需要 PostgreSQL 的 xmax。
struct TupleVersion {
    Row values;
    std::uint64_t xmin{};
};

// created_by 用于演示 PostgreSQL 的事务型 DDL：提交后归零，回滚时删除该表。
struct HeapTable {
    TableSchema schema;
    std::uint64_t created_by{};
    std::vector<TupleVersion> versions;
};

// parser 只区分字面量类型，具体列数和列类型必须结合 schema 检查。
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

// SELECT 结果表头直接来自 catalog 中保存的 schema。
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
    // 教学实现用一把互斥锁保护共享状态；真实 PostgreSQL 使用更细粒度的锁和闩锁。
    std::mutex mutex;
    std::uint64_t next_transaction_id{1};
    std::uint64_t next_backend_pid{10001};
    std::unordered_map<std::uint64_t, TransactionStatus> transaction_status;
    std::unordered_map<std::string, HeapTable> tables;

    // 分配单调递增 xid，并立即将其登记为进行中。
    std::uint64_t begin_transaction() {
        const auto id = next_transaction_id++;
        transaction_status[id] = TransactionStatus::in_progress;
        return id;
    }

    // READ COMMITTED 在每条语句开始时重新收集活动事务，因此同一事务的
    // 两次 SELECT 之间可以看到其他事务新提交的数据。
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

    // MVCC 可见性规则（简化版）：自己的写入可见；其他版本必须已经提交、
    // xid 小于快照上界，并且创建者不在快照的活动事务集合中。
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

    // 尚未提交的 CREATE TABLE 只对创建它的当前事务可见。
    bool table_visible(const HeapTable& table, std::optional<std::uint64_t> self) const {
        return table.created_by == 0 || (self && table.created_by == *self);
    }

    // 提交数据版本只需修改事务状态；事务内创建的表同时转为全局可见。
    void commit_transaction(std::uint64_t id) {
        transaction_status[id] = TransactionStatus::committed;
        for (auto& [name, table] : tables) {
            (void)name;
            if (table.created_by == id) {
                table.created_by = 0;
            }
        }
    }

    // 回滚的数据 tuple 暂留给 VACUUM；事务内创建的 catalog 项则立即移除。
    void abort_transaction(std::uint64_t id) {
        transaction_status[id] = TransactionStatus::aborted;
        std::erase_if(tables, [id](const auto& item) { return item.second.created_by == id; });
    }
};

PostgresDatabase::PostgresDatabase() : impl_(std::make_unique<Impl>()) {}
PostgresDatabase::~PostgresDatabase() = default;

std::unique_ptr<PostgresSession> PostgresDatabase::connect() {
    std::lock_guard lock(impl_->mutex);
    // 逻辑 PID 只用于展示连接边界，不对应真实操作系统进程号。
    return std::unique_ptr<PostgresSession>(new PostgresSession(*this, impl_->next_backend_pid++));
}

PostgresSession::PostgresSession(PostgresDatabase& database, std::uint64_t backend_pid)
    : database_(&database), backend_pid_(backend_pid) {}

PostgresSession::~PostgresSession() {
    // 客户端断开连接时，未提交事务不能泄漏为永久的 in_progress 状态。
    if (database_ != nullptr) {
        database_->rollback_on_disconnect(*this);
    }
}

QueryResult PostgresSession::execute(const std::string& sql) {
    const auto parsed = parse_sql(sql);
    if (!parsed.ok()) {
        // PostgreSQL 将事务块内的语法/执行错误都视为当前事务失败。
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
    // 所有 catalog、事务表和 heap 操作在同一个临界区内完成，保证示例确定性。
    std::lock_guard lock(impl_->mutex);

    // std::visit 根据 AST 类型分派执行路径，效果类似一个极小的 utility command/executor。
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
                // 即使用户没有显式 BEGIN，单条写语句内部也使用一个完整事务。
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
            // INSERT 不覆盖已有内容，而是向 heap 末尾追加带 xmin 的新版本。
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
            // PostgreSQL 默认 READ COMMITTED：每条语句获取一个新快照。
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
                // 当前模型没有 UPDATE/DELETE，唯一的 dead tuple 来源是回滚事务。
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
