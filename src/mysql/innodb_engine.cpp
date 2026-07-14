#include "minidb/mysql/storage_engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::mysql {
namespace {

// 引擎内部保留事务最终状态，供行版本可见性判断使用。
enum class TransactionStatus { in_progress, committed, rolled_back };

// 教学版 InnoDB 行版本只记录创建事务；没有 UPDATE/DELETE 时无需 undo 指针。
struct RowVersion {
    Row values;
    std::uint64_t created_by{};
};

// InnoDBEngine 展示事务型引擎应如何实现统一 StorageEngine 接口。
class InnoDBEngine final : public StorageEngine {
public:
    std::string name() const override { return "INNODB"; }
    bool transactional() const override { return true; }
    std::string description() const override {
        return "事务型行存储：MVCC、一致性读、COMMIT/ROLLBACK";
    }

    QueryResult create_table(const TableSchema& schema) override {
        // schema 由 MySQL Server catalog 保存，引擎这里只建立数据容器。
        tables_.emplace(schema.name, std::vector<RowVersion>{});
        return QueryResult::success();
    }

    QueryResult insert(const std::string& table,
                       const Row& row,
                       std::uint64_t transaction_id) override {
        const auto iterator = tables_.find(table);
        if (iterator == tables_.end()) {
            return QueryResult::error("InnoDB 找不到表: " + table);
        }
        // 写入先以当前事务版本存在，只有提交后才对其他事务可见。
        iterator->second.push_back(RowVersion{row, transaction_id});
        return QueryResult::success();
    }

    std::vector<Row> select(const std::string& table, const ReadView& view) const override {
        std::vector<Row> rows;
        const auto iterator = tables_.find(table);
        if (iterator == tables_.end()) {
            return rows;
        }
        for (const auto& version : iterator->second) {
            // 同一个表可能同时包含已提交、进行中和已回滚事务的版本。
            if (visible(version, view)) {
                rows.push_back(version.values);
            }
        }
        return rows;
    }

    void begin(std::uint64_t transaction_id) override {
        transaction_status_[transaction_id] = TransactionStatus::in_progress;
    }

    void commit(std::uint64_t transaction_id) override {
        transaction_status_[transaction_id] = TransactionStatus::committed;
    }

    void rollback(std::uint64_t transaction_id) override {
        // 这里只标记回滚，不立即擦除版本，用来类比真实 InnoDB 的 undo/purge 分工。
        transaction_status_[transaction_id] = TransactionStatus::rolled_back;
    }

private:
    // 一致性读规则：自己的写入可见；其他事务必须已提交、早于视图上界，
    // 并且在创建 ReadView 时不属于活动事务。
    bool visible(const RowVersion& version, const ReadView& view) const {
        if (view.self && version.created_by == *view.self) {
            return true;
        }
        const auto status = transaction_status_.find(version.created_by);
        return status != transaction_status_.end() &&
               status->second == TransactionStatus::committed &&
               version.created_by < view.low_limit_id &&
               !view.active_transactions.contains(version.created_by);
    }

    std::unordered_map<std::string, std::vector<RowVersion>> tables_;
    // 所有 Server 事务事件都会广播给引擎，因此这里能查询版本创建者的状态。
    std::unordered_map<std::uint64_t, TransactionStatus> transaction_status_;
};

}  // namespace

std::unique_ptr<StorageEngine> make_innodb_engine() {
    // 通过抽象类型返回，Server 层无需包含具体 InnoDBEngine 定义。
    return std::make_unique<InnoDBEngine>();
}

}  // namespace minidb::mysql
