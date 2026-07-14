#include "minidb/mysql/storage_engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::mysql {
namespace {

enum class TransactionStatus { in_progress, committed, rolled_back };

struct RowVersion {
    Row values;
    std::uint64_t created_by{};
};

class InnoDBEngine final : public StorageEngine {
public:
    std::string name() const override { return "INNODB"; }
    bool transactional() const override { return true; }
    std::string description() const override {
        return "事务型行存储：MVCC、一致性读、COMMIT/ROLLBACK";
    }

    QueryResult create_table(const TableSchema& schema) override {
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
        transaction_status_[transaction_id] = TransactionStatus::rolled_back;
    }

private:
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
    std::unordered_map<std::uint64_t, TransactionStatus> transaction_status_;
};

}  // namespace

std::unique_ptr<StorageEngine> make_innodb_engine() {
    return std::make_unique<InnoDBEngine>();
}

}  // namespace minidb::mysql
