#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace minidb {

enum class DataType {
    integer,
    text,
};

using Cell = std::variant<std::int64_t, std::string>;
using Row = std::vector<Cell>;

struct Column {
    std::string name;
    DataType type;
};

struct TableSchema {
    std::string name;
    std::vector<Column> columns;
};

std::string cell_to_string(const Cell& cell);
std::string data_type_name(DataType type);
std::string ascii_lower(std::string text);

}  // namespace minidb
