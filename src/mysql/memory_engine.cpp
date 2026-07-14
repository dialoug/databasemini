#include "minidb/mysql/storage_engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::mysql {
namespace {

class MemoryEngine final : public StorageEngine {
public:
    std::string name() const override { return "MEMORY"; }
    bool transactional() const override { return false; }
    std::string description() const override {
        return "非事务型内存表：写入立即可见，ROLLBACK 不撤销";
    }

    QueryResult create_table(const TableSchema& schema) override {
        tables_.emplace(schema.name, std::vector<Row>{});
        return QueryResult::success();
    }

    QueryResult insert(const std::string& table,
                       const Row& row,
                       std::uint64_t transaction_id) override {
        (void)transaction_id;
        const auto iterator = tables_.find(table);
        if (iterator == tables_.end()) {
            return QueryResult::error("MEMORY 找不到表: " + table);
        }
        iterator->second.push_back(row);
        return QueryResult::success();
    }

    std::vector<Row> select(const std::string& table, const ReadView& view) const override {
        (void)view;
        const auto iterator = tables_.find(table);
        return iterator == tables_.end() ? std::vector<Row>{} : iterator->second;
    }

    void begin(std::uint64_t transaction_id) override { (void)transaction_id; }
    void commit(std::uint64_t transaction_id) override { (void)transaction_id; }
    void rollback(std::uint64_t transaction_id) override { (void)transaction_id; }

private:
    std::unordered_map<std::string, std::vector<Row>> tables_;
};

}  // namespace

std::unique_ptr<StorageEngine> make_memory_engine() {
    return std::make_unique<MemoryEngine>();
}

}  // namespace minidb::mysql
