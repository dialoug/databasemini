#pragma once

#include "minidb/common/types.hpp"

#include <string>
#include <vector>

namespace minidb {

// 所有语句统一返回 QueryResult：既能表达简单状态，也能表达 SELECT 结果集。
// 这相当于教学版的数据库协议响应，但没有实现真实 PostgreSQL/MySQL 协议。
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

    // 将结果渲染成适合终端阅读的表格，并处理中文字符的显示宽度。
    [[nodiscard]] std::string format() const;
};

}  // namespace minidb
