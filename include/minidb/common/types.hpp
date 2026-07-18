#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace minidb {

// 当前教学项目只实现两种数据类型，以便把注意力放在数据库架构上。
enum class DataType {
    integer,
    text,
};

// Cell 是单个字段，Row 是一行数据。variant 让类型错误可在执行阶段被检查。
using Cell = std::variant<std::int64_t, std::string>;
using Row = std::vector<Cell>;

// 列定义包含名称和逻辑类型。
struct Column {
    std::string name;
    DataType type;
};

// 表结构由 catalog 保存，具体的行数据由各自存储层保存。
struct TableSchema {
    std::string name;
    std::vector<Column> columns;
};

// 下列工具函数主要供两套关系型实现共用，不包含数据库特有语义。
std::string cell_to_string(const Cell& cell);
std::string data_type_name(DataType type);
std::string ascii_lower(std::string text);

}  // namespace minidb
