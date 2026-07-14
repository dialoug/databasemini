#pragma once

#include "minidb/common/result.hpp"
#include "minidb/common/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace minidb::mysql {

// InnoDB 一致性读视图：创建视图时的事务上界、活动事务集合和当前事务。
struct ReadView {
    std::uint64_t low_limit_id{};
    std::unordered_set<std::uint64_t> active_transactions;
    std::optional<std::uint64_t> self;
};

// MySQL Server 层通过统一 handler 风格接口访问不同存储引擎。
// 新增教学引擎时只需实现此接口并在 MysqlDatabase 中注册。
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool transactional() const = 0;
    [[nodiscard]] virtual std::string description() const = 0;

    // Server 层负责解析和类型检查，引擎只负责表数据和事务可见性。
    virtual QueryResult create_table(const TableSchema& schema) = 0;
    virtual QueryResult insert(const std::string& table,
                               const Row& row,
                               std::uint64_t transaction_id) = 0;
    virtual std::vector<Row> select(const std::string& table, const ReadView& view) const = 0;

    virtual void begin(std::uint64_t transaction_id) = 0;
    virtual void commit(std::uint64_t transaction_id) = 0;
    virtual void rollback(std::uint64_t transaction_id) = 0;
};

// 工厂函数避免公开具体引擎类，使 Server 层只依赖抽象接口。
std::unique_ptr<StorageEngine> make_innodb_engine();
std::unique_ptr<StorageEngine> make_memory_engine();

}  // namespace minidb::mysql
