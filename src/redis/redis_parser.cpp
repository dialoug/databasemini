#include "minidb/redis/redis_parser.hpp"

#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb::redis {
namespace {

enum class TokenKind { value, semicolon, end };

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
            } else if (character == ';') {
                tokens.push_back(Token{TokenKind::semicolon, ";"});
                ++position_;
            } else if (character == '\'' || character == '"') {
                tokens.push_back(scan_quoted(character));
            } else {
                tokens.push_back(scan_unquoted());
            }
        }
        tokens.push_back(Token{TokenKind::end, {}});
        return tokens;
    }

private:
    Token scan_unquoted() {
        const auto start = position_;
        while (position_ < input_.size() &&
               !std::isspace(static_cast<unsigned char>(input_[position_])) &&
               input_[position_] != ';') {
            ++position_;
        }
        return Token{TokenKind::value,
                     std::string(input_.substr(start, position_ - start))};
    }

    Token scan_quoted(char quote) {
        ++position_;
        std::string value;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == quote) {
                return Token{TokenKind::value, std::move(value)};
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
        if (std::toupper(static_cast<unsigned char>(value[index])) != keyword[index]) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement parse() {
        const auto command = expect_value("命令");
        if (equals_keyword(command, "SET")) {
            SetStatement statement{expect_value("key"), expect_value("value")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "GET")) {
            GetStatement statement{expect_value("key")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "DEL")) {
            DeleteStatement statement{expect_value("key")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "EXISTS")) {
            ExistsStatement statement{expect_value("key")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "INCR")) {
            IncrementStatement statement{expect_value("key")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "KEYS")) {
            KeysStatement statement{expect_value("pattern")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "PING")) {
            std::optional<std::string> message;
            if (peek().kind == TokenKind::value) {
                message = advance().text;
            }
            finish();
            return PingStatement{std::move(message)};
        }
        if (equals_keyword(command, "TYPE")) {
            TypeStatement statement{expect_value("key")};
            finish();
            return statement;
        }
        if (equals_keyword(command, "DBSIZE")) {
            finish();
            return DbSizeStatement{};
        }
        if (equals_keyword(command, "MULTI")) {
            finish();
            return MultiStatement{};
        }
        if (equals_keyword(command, "EXEC")) {
            finish();
            return ExecStatement{};
        }
        if (equals_keyword(command, "DISCARD")) {
            finish();
            return DiscardStatement{};
        }
        if (equals_keyword(command, "SHOW")) {
            const auto subject = expect_value("ARCHITECTURE");
            if (!equals_keyword(subject, "ARCHITECTURE")) {
                throw std::runtime_error("SHOW 仅支持 SHOW ARCHITECTURE");
            }
            finish();
            return ShowArchitectureStatement{};
        }
        if (equals_keyword(command, "HELP")) {
            finish();
            return HelpStatement{};
        }
        if (equals_keyword(command, "QUIT") || equals_keyword(command, "EXIT")) {
            finish();
            return QuitStatement{};
        }
        throw std::runtime_error("不支持的 Redis 命令；输入 HELP 查看支持的语法");
    }

private:
    const Token& peek() const { return tokens_[position_]; }
    Token advance() { return tokens_[position_++]; }

    std::string expect_value(std::string_view description) {
        if (peek().kind != TokenKind::value) {
            throw std::runtime_error("期望" + std::string(description));
        }
        return advance().text;
    }

    void finish() {
        if (peek().kind == TokenKind::semicolon) {
            advance();
        }
        if (peek().kind != TokenKind::end) {
            throw std::runtime_error("命令末尾存在多余内容: " + peek().text);
        }
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

}  // namespace minidb::redis
