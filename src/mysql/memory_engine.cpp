#include "minidb/mysql/storage_engine.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::mysql {
namespace {

// MEMORY 是故意设计的非事务型对照组：相同 SQL 经过相同 Server 层，
// 但最终的提交/回滚语义由存储引擎能力决定。
class MemoryEngine final : public StorageEngine {
public:
    std::string name() const override { return "MEMORY"; }
    bool transactional() const override { return false; }
    std::string description() const override {
        return "非事务型内存表：写入立即可见，ROLLBACK 不撤销";
    }

    QueryResult create_table(const TableSchema& schema) override {
        // 数据直接保存在进程内 vector 中，程序退出后全部消失。
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
        // 不创建行版本，写入后立即对所有会话可见。
        iterator->second.push_back(row);
        return QueryResult::success();
    }

    std::vector<Row> select(const std::string& table, const ReadView& view) const override {
        // 非事务引擎不使用一致性读视图。
        (void)view;
        const auto iterator = tables_.find(table);
        return iterator == tables_.end() ? std::vector<Row>{} : iterator->second;
    }

    // 事务生命周期回调全部为空，因此 ROLLBACK 不会撤销已经追加的行。
    void begin(std::uint64_t transaction_id) override { (void)transaction_id; }
    void commit(std::uint64_t transaction_id) override { (void)transaction_id; }
    void rollback(std::uint64_t transaction_id) override { (void)transaction_id; }

private:
    std::unordered_map<std::string, std::vector<Row>> tables_;
};

}  // namespace

std::unique_ptr<StorageEngine> make_memory_engine() {
    // 工厂返回基类指针，支持 Server 层用统一容器管理多种引擎。
    return std::make_unique<MemoryEngine>();
}

}  // namespace minidb::mysql
