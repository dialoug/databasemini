#include "minidb/mongo/mongo_parser.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb::mongo {
namespace {

enum class TokenKind { word, number, string, symbol, end };

struct Token {
    TokenKind kind;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {
        if (input_.size() >= 3 &&
            static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB &&
            static_cast<unsigned char>(input_[2]) == 0xBF) {
            position_ = 3;
        }
    }

    std::vector<Token> scan() {
        std::vector<Token> tokens;
        while (position_ < input_.size()) {
            const auto character = input_[position_];
            if (std::isspace(static_cast<unsigned char>(character))) {
                ++position_;
            } else if (std::isalpha(static_cast<unsigned char>(character)) ||
                       character == '_' || character == '$') {
                tokens.push_back(scan_word());
            } else if (std::isdigit(static_cast<unsigned char>(character)) ||
                       (character == '-' && position_ + 1 < input_.size() &&
                        std::isdigit(static_cast<unsigned char>(input_[position_ + 1])))) {
                tokens.push_back(scan_number());
            } else if (character == '\'' || character == '"') {
                tokens.push_back(scan_string(character));
            } else if (std::string_view(".(){}[],:;").find(character) !=
                       std::string_view::npos) {
                tokens.push_back(Token{TokenKind::symbol, std::string(1, character)});
                ++position_;
            } else {
                throw std::runtime_error("无法识别的字符: " + std::string(1, character));
            }
        }
        tokens.push_back(Token{TokenKind::end, {}});
        return tokens;
    }

private:
    Token scan_word() {
        const auto start = position_;
        while (position_ < input_.size()) {
            const auto character = input_[position_];
            if (!std::isalnum(static_cast<unsigned char>(character)) &&
                character != '_' && character != '$') {
                break;
            }
            ++position_;
        }
        return Token{TokenKind::word, std::string(input_.substr(start, position_ - start))};
    }

    Token scan_number() {
        const auto start = position_++;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return Token{TokenKind::number, std::string(input_.substr(start, position_ - start))};
    }

    Token scan_string(char quote) {
        ++position_;
        std::string value;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == quote) {
                return Token{TokenKind::string, std::move(value)};
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                throw std::runtime_error("字符串末尾存在不完整的转义");
            }
            const auto escaped = input_[position_++];
            switch (escaped) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case '\\': value.push_back('\\'); break;
                case '\'': value.push_back('\''); break;
                case '"': value.push_back('"'); break;
                default:
                    throw std::runtime_error("不支持的字符串转义: \\" +
                                             std::string(1, escaped));
            }
        }
        throw std::runtime_error("字符串缺少结束引号");
    }

    std::string_view input_;
    std::size_t position_{0};
};

bool equals_keyword(const std::string& value, std::string_view keyword) {
    if (value.size() != keyword.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(value[index])) !=
            std::toupper(static_cast<unsigned char>(keyword[index]))) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement parse() {
        if (match_keyword("DB")) {
            return parse_database_command();
        }
        if (match_keyword("BEGIN")) {
            finish();
            return BeginStatement{};
        }
        if (match_keyword("COMMIT")) {
            finish();
            return CommitStatement{};
        }
        if (match_keyword("ROLLBACK")) {
            finish();
            return RollbackStatement{};
        }
        if (match_keyword("SESSION")) {
            return parse_session_command();
        }
        if (match_keyword("SHOW")) {
            if (match_keyword("COLLECTIONS")) {
                finish();
                return ShowCollectionsStatement{};
            }
            expect_keyword("ARCHITECTURE");
            finish();
            return ShowArchitectureStatement{};
        }
        if (match_keyword("HELP")) {
            finish();
            return HelpStatement{};
        }
        if (match_keyword("QUIT") || match_keyword("EXIT")) {
            finish();
            return QuitStatement{};
        }
        throw std::runtime_error("不支持的 MongoDB 命令；输入 HELP 查看支持的语法");
    }

private:
    Statement parse_database_command() {
        expect_symbol(".");
        if (match_keyword("CREATECOLLECTION")) {
            expect_symbol("(");
            const auto collection = expect_string("collection 名称");
            expect_symbol(")");
            finish();
            return CreateCollectionStatement{collection};
        }

        const auto collection = expect_word("collection 名称");
        expect_symbol(".");
        if (match_keyword("INSERTONE")) {
            expect_symbol("(");
            auto document = parse_document();
            expect_symbol(")");
            finish();
            return InsertOneStatement{collection, std::move(document)};
        }
        if (match_keyword("FIND")) {
            expect_symbol("(");
            Document filter;
            if (!check_symbol(")")) {
                filter = parse_document();
            }
            expect_symbol(")");
            finish();
            return FindStatement{collection, std::move(filter)};
        }
        throw std::runtime_error("collection 仅支持 insertOne() 和 find()");
    }

    Statement parse_session_command() {
        expect_symbol(".");
        if (match_keyword("STARTTRANSACTION")) {
            expect_empty_call();
            finish();
            return BeginStatement{};
        }
        if (match_keyword("COMMITTRANSACTION")) {
            expect_empty_call();
            finish();
            return CommitStatement{};
        }
        if (match_keyword("ABORTTRANSACTION")) {
            expect_empty_call();
            finish();
            return RollbackStatement{};
        }
        throw std::runtime_error(
            "session 仅支持 startTransaction()、commitTransaction() 和 abortTransaction()");
    }

    void expect_empty_call() {
        expect_symbol("(");
        expect_symbol(")");
    }

    Document parse_document() {
        expect_symbol("{");
        Document document;
        if (match_symbol("}")) {
            return document;
        }
        while (true) {
            std::string key;
            if (peek().kind == TokenKind::word || peek().kind == TokenKind::string) {
                key = advance().text;
            } else {
                throw std::runtime_error("文档字段名必须是标识符或字符串");
            }
            expect_symbol(":");
            if (!document.emplace(key, parse_value()).second) {
                throw std::runtime_error("重复文档字段: " + key);
            }
            if (!match_symbol(",")) {
                break;
            }
        }
        expect_symbol("}");
        return document;
    }

    BsonArray parse_array() {
        expect_symbol("[");
        BsonArray array;
        if (match_symbol("]")) {
            return array;
        }
        while (true) {
            array.push_back(parse_value());
            if (!match_symbol(",")) {
                break;
            }
        }
        expect_symbol("]");
        return array;
    }

    BsonValue parse_value() {
        if (peek().kind == TokenKind::number) {
            const auto token = advance().text;
            std::int64_t value{};
            const auto result =
                std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
                throw std::runtime_error("整数超出范围: " + token);
            }
            return BsonValue(value);
        }
        if (peek().kind == TokenKind::string) {
            return BsonValue(advance().text);
        }
        if (check_symbol("{")) {
            return BsonValue(parse_document());
        }
        if (check_symbol("[")) {
            return BsonValue(parse_array());
        }
        if (match_keyword("TRUE")) {
            return BsonValue(true);
        }
        if (match_keyword("FALSE")) {
            return BsonValue(false);
        }
        if (match_keyword("NULL")) {
            return BsonValue(nullptr);
        }
        throw std::runtime_error("文档值仅支持 null、bool、整数、字符串、对象和数组");
    }

    void finish() {
        match_symbol(";");
        if (peek().kind != TokenKind::end) {
            throw std::runtime_error("命令末尾存在多余内容: " + peek().text);
        }
    }

    const Token& peek() const { return tokens_[position_]; }
    Token advance() { return tokens_[position_++]; }

    bool match_keyword(std::string_view keyword) {
        if (peek().kind == TokenKind::word && equals_keyword(peek().text, keyword)) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect_keyword(std::string_view keyword) {
        if (!match_keyword(keyword)) {
            throw std::runtime_error("期望关键字 " + std::string(keyword));
        }
    }

    bool check_symbol(std::string_view symbol) const {
        return peek().kind == TokenKind::symbol && peek().text == symbol;
    }

    bool match_symbol(std::string_view symbol) {
        if (check_symbol(symbol)) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect_symbol(std::string_view symbol) {
        if (!match_symbol(symbol)) {
            throw std::runtime_error("期望符号 " + std::string(symbol));
        }
    }

    std::string expect_word(std::string_view description) {
        if (peek().kind != TokenKind::word) {
            throw std::runtime_error("期望" + std::string(description));
        }
        return advance().text;
    }

    std::string expect_string(std::string_view description) {
        if (peek().kind != TokenKind::string) {
            throw std::runtime_error("期望字符串形式的" + std::string(description));
        }
        return advance().text;
    }

    std::vector<Token> tokens_;
    std::size_t position_{0};
};

}  // namespace

ParseResult parse_command(const std::string& command) {
    try {
        Lexer lexer(command);
        auto tokens = lexer.scan();
        if (tokens.size() == 1) {
            return ParseResult{std::nullopt, "命令不能为空"};
        }
        return ParseResult{Parser(std::move(tokens)).parse(), {}};
    } catch (const std::exception& exception) {
        return ParseResult{std::nullopt, exception.what()};
    }
}

}  // namespace minidb::mongo
