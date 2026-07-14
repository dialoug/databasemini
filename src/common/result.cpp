#include "minidb/common/result.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace minidb {
namespace {

std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80) {
            ++width;
            ++index;
            continue;
        }
        // The REPL primarily prints Chinese. Treat each non-ASCII UTF-8 code point as
        // two terminal cells so the educational result tables stay readable.
        std::size_t sequence_length = 1;
        if ((byte & 0xE0) == 0xC0) {
            sequence_length = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            sequence_length = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            sequence_length = 4;
        }
        index += std::min(sequence_length, text.size() - index);
        width += 2;
    }
    return width;
}

void append_padded(std::ostringstream& output, const std::string& value, std::size_t width) {
    output << value;
    const auto value_width = display_width(value);
    if (value_width < width) {
        output << std::string(width - value_width, ' ');
    }
}

}  // namespace

std::string cell_to_string(const Cell& cell) {
    return std::visit([](const auto& value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::string>) {
            return value;
        } else {
            return std::to_string(value);
        }
    }, cell);
}

std::string data_type_name(DataType type) {
    return type == DataType::integer ? "INT" : "TEXT";
}

std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

QueryResult QueryResult::success(std::string message) {
    return QueryResult{true, std::move(message), {}, {}};
}

QueryResult QueryResult::table(std::vector<std::string> columns,
                               std::vector<Row> rows,
                               std::string message) {
    return QueryResult{true, std::move(message), std::move(columns), std::move(rows)};
}

QueryResult QueryResult::error(std::string message) {
    return QueryResult{false, std::move(message), {}, {}};
}

std::string QueryResult::format() const {
    if (!ok) {
        return "ERROR: " + message;
    }
    if (columns.empty()) {
        return message.empty() ? "OK" : message;
    }

    std::vector<std::vector<std::string>> rendered_rows;
    rendered_rows.reserve(rows.size());
    std::vector<std::size_t> widths;
    widths.reserve(columns.size());
    for (const auto& column : columns) {
        widths.push_back(display_width(column));
    }
    for (const auto& row : rows) {
        std::vector<std::string> rendered;
        rendered.reserve(row.size());
        for (std::size_t index = 0; index < row.size(); ++index) {
            rendered.push_back(cell_to_string(row[index]));
            if (index < widths.size()) {
                widths[index] = std::max(widths[index], display_width(rendered.back()));
            }
        }
        rendered_rows.push_back(std::move(rendered));
    }

    std::ostringstream output;
    const auto divider = [&]() {
        output << '+';
        for (const auto width : widths) {
            output << std::string(width + 2, '-') << '+';
        }
        output << '\n';
    };
    divider();
    output << '|';
    for (std::size_t index = 0; index < columns.size(); ++index) {
        output << ' ';
        append_padded(output, columns[index], widths[index]);
        output << " |";
    }
    output << '\n';
    divider();
    for (const auto& row : rendered_rows) {
        output << '|';
        for (std::size_t index = 0; index < columns.size(); ++index) {
            const std::string value = index < row.size() ? row[index] : "";
            output << ' ';
            append_padded(output, value, widths[index]);
            output << " |";
        }
        output << '\n';
    }
    divider();
    output << (message.empty() ? std::to_string(rows.size()) + " row(s)" : message);
    return output.str();
}

}  // namespace minidb
