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

struct ReadView {
    std::uint64_t low_limit_id{};
    std::unordered_set<std::uint64_t> active_transactions;
    std::optional<std::uint64_t> self;
};

class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool transactional() const = 0;
    [[nodiscard]] virtual std::string description() const = 0;

    virtual QueryResult create_table(const TableSchema& schema) = 0;
    virtual QueryResult insert(const std::string& table,
                               const Row& row,
                               std::uint64_t transaction_id) = 0;
    virtual std::vector<Row> select(const std::string& table, const ReadView& view) const = 0;

    virtual void begin(std::uint64_t transaction_id) = 0;
    virtual void commit(std::uint64_t transaction_id) = 0;
    virtual void rollback(std::uint64_t transaction_id) = 0;
};

std::unique_ptr<StorageEngine> make_innodb_engine();
std::unique_ptr<StorageEngine> make_memory_engine();

}  // namespace minidb::mysql
