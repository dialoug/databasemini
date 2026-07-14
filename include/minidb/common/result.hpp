#pragma once

#include "minidb/common/types.hpp"

#include <string>
#include <vector>

namespace minidb {

struct QueryResult {
    bool ok{true};
    std::string message;
    std::vector<std::string> columns;
    std::vector<Row> rows;

    static QueryResult success(std::string message = {});
    static QueryResult table(std::vector<std::string> columns,
                             std::vector<Row> rows,
                             std::string message = {});
    static QueryResult error(std::string message);

    [[nodiscard]] std::string format() const;
};

}  // namespace minidb
