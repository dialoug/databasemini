#include "minidb/mongo/bson.hpp"

#include <iomanip>
#include <sstream>
#include <type_traits>

namespace minidb::mongo {
namespace {

std::string quote_json(const std::string& text) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : text) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character) << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

std::string array_to_json(const BsonArray& array) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < array.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << bson_to_json(array[index]);
    }
    output << ']';
    return output.str();
}

}  // namespace

std::string bson_to_json(const BsonValue& value) {
    return std::visit([](const auto& stored) -> std::string {
        using Value = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
            return "null";
        } else if constexpr (std::is_same_v<Value, bool>) {
            return stored ? "true" : "false";
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
            return std::to_string(stored);
        } else if constexpr (std::is_same_v<Value, std::string>) {
            return quote_json(stored);
        } else if constexpr (std::is_same_v<Value, Document>) {
            return document_to_json(stored);
        } else {
            return array_to_json(stored);
        }
    }, value.value);
}

std::string document_to_json(const Document& document) {
    std::ostringstream output;
    output << '{';
    std::size_t index = 0;
    for (const auto& [key, value] : document) {
        if (index++ != 0) {
            output << ", ";
        }
        output << quote_json(key) << ": " << bson_to_json(value);
    }
    output << '}';
    return output.str();
}

}  // namespace minidb::mongo
