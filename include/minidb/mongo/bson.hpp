#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace minidb::mongo {

// BSONValue 是教学版 BSON：保留文档数据库最关键的嵌套对象和数组，
// 但只实现 null、bool、int64、string、document、array 六种值类型。
struct BsonValue {
    using Document = std::map<std::string, BsonValue>;
    using Array = std::vector<BsonValue>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::int64_t, std::string, Document, Array>;

    BsonValue() : value(nullptr) {}
    BsonValue(std::nullptr_t) : value(nullptr) {}
    BsonValue(bool input) : value(input) {}
    BsonValue(std::int64_t input) : value(input) {}
    BsonValue(std::string input) : value(std::move(input)) {}
    BsonValue(const char* input) : value(std::string(input)) {}
    BsonValue(Document input) : value(std::move(input)) {}
    BsonValue(Array input) : value(std::move(input)) {}

    Storage value;

    bool operator==(const BsonValue&) const = default;
};

using Document = BsonValue::Document;
using BsonArray = BsonValue::Array;

// 将教学版 BSON 渲染为 Mongo shell 易读的 JSON 形式。
std::string bson_to_json(const BsonValue& value);
std::string document_to_json(const Document& document);

}  // namespace minidb::mongo
