#include "minidb/common/sql_parser.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb {
namespace {

// lexer 只区分解析当前 SQL 子集所需的五类 token。
enum class TokenKind { word, number, string, symbol, end };

struct Token {
    TokenKind kind;
    std::string text;
};

// Lexer 负责字符级扫描，不判断 CREATE、SELECT 等语句结构。
class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {
        // 某些 Windows shell 向原生程序管道输入文本时会在首行添加 UTF-8 BOM。
        if (input_.size() >= 3 &&
            static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB &&
            static_cast<unsigned char>(input_[2]) == 0xBF) {
            position_ = 3;
        }
    }

    std::vector<Token> scan() {
        std::vector<Token> tokens;
        // 每轮至少消费一个字符，遇到非法字符立即给出位置附近的可读错误。
        while (position_ < input_.size()) {
            const auto character = input_[position_];
            if (std::isspace(static_cast<unsigned char>(character))) {
                ++position_;
            } else if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
                tokens.push_back(scan_word());
            } else if (std::isdigit(static_cast<unsigned char>(character)) ||
                       (character == '-' && position_ + 1 < input_.size() &&
                        std::isdigit(static_cast<unsigned char>(input_[position_ + 1])))) {
                tokens.push_back(scan_number());
            } else if (character == '\'') {
                tokens.push_back(scan_string());
            } else if (character == '(' || character == ')' || character == ',' ||
                       character == '*' || character == '=' || character == ';') {
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
        // 标识符和关键字都先扫描为 word，是否为关键字由 Parser 再判断。
        const auto start = position_;
        while (position_ < input_.size()) {
            const auto character = input_[position_];
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') {
                break;
            }
            ++position_;
        }
        return Token{TokenKind::word, std::string(input_.substr(start, position_ - start))};
    }

    Token scan_number() {
        // 负号只有紧跟数字时才被当作整数的一部分。
        const auto start = position_++;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return Token{TokenKind::number, std::string(input_.substr(start, position_ - start))};
    }

    Token scan_string() {
        ++position_;
        std::string value;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == '\'') {
                // SQL 使用两个连续单引号表示字符串中的一个单引号。
                if (position_ < input_.size() && input_[position_] == '\'') {
                    value.push_back('\'');
                    ++position_;
                    continue;
                }
                return Token{TokenKind::string, std::move(value)};
            }
            value.push_back(character);
        }
        throw std::runtime_error("字符串缺少结束单引号");
    }

    std::string_view input_;
    std::size_t position_{0};
};

// SQL 关键字大小写不敏感；标识符大小写规则由具体数据库执行器决定。
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

// Parser 通过 position_ 在 token 数组中前进，并生成强类型 AST。
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement parse() {
        // 首个关键字决定后续采用哪条语法分支。
        if (match_keyword("CREATE")) {
            return parse_create();
        }
        if (match_keyword("INSERT")) {
            return parse_insert();
        }
        if (match_keyword("SELECT")) {
            return parse_select();
        }
        if (match_keyword("BEGIN") || match_keyword("START")) {
            if (previous_keyword("START")) {
                expect_keyword("TRANSACTION");
            }
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
        if (match_keyword("SHOW")) {
            if (match_keyword("ARCHITECTURE")) {
                finish();
                return ShowArchitectureStatement{};
            }
            expect_keyword("ENGINES");
            finish();
            return ShowEnginesStatement{};
        }
        if (match_keyword("VACUUM")) {
            std::optional<std::string> table;
            if (peek().kind == TokenKind::word) {
                table = advance().text;
            }
            finish();
            return VacuumStatement{std::move(table)};
        }
        if (match_keyword("HELP")) {
            finish();
            return HelpStatement{};
        }
        if (match_keyword("QUIT") || match_keyword("EXIT")) {
            finish();
            return QuitStatement{};
        }
        throw std::runtime_error("不支持的 SQL；输入 HELP 查看支持的语法");
    }

private:
    Statement parse_create() {
        // CREATE TABLE t (id INT, name TEXT) [ENGINE=...]
        expect_keyword("TABLE");
        TableSchema schema;
        schema.name = expect_word("表名");
        expect_symbol("(");
        do {
            Column column;
            column.name = expect_word("列名");
            if (match_keyword("INT") || match_keyword("INTEGER")) {
                column.type = DataType::integer;
            } else if (match_keyword("TEXT") || match_keyword("VARCHAR")) {
                column.type = DataType::text;
            } else {
                throw std::runtime_error("列类型必须是 INT、INTEGER、TEXT 或 VARCHAR");
            }
            schema.columns.push_back(std::move(column));
        } while (match_symbol(","));
        expect_symbol(")");

        std::string engine;
        if (match_keyword("ENGINE")) {
            match_symbol("=");
            engine = expect_word("存储引擎名");
        }
        finish();
        return CreateTableStatement{std::move(schema), std::move(engine)};
    }

    Statement parse_insert() {
        // INSERT INTO t VALUES (...)；本项目暂不支持指定列名。
        expect_keyword("INTO");
        InsertStatement statement;
        statement.table = expect_word("表名");
        expect_keyword("VALUES");
        expect_symbol("(");
        do {
            if (peek().kind == TokenKind::number) {
                const auto token = advance().text;
                std::int64_t value{};
                const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
                if (result.ec != std::errc{}) {
                    throw std::runtime_error("整数超出范围: " + token);
                }
                statement.values.emplace_back(value);
            } else if (peek().kind == TokenKind::string) {
                statement.values.emplace_back(advance().text);
            } else {
                throw std::runtime_error("VALUES 仅支持整数和单引号字符串");
            }
        } while (match_symbol(","));
        expect_symbol(")");
        finish();
        return statement;
    }

    Statement parse_select() {
        // 为突出存储和事务路径，查询语法刻意只支持 SELECT * FROM t。
        expect_symbol("*");
        expect_keyword("FROM");
        SelectStatement statement{expect_word("表名")};
        finish();
        return statement;
    }

    void finish() {
        // 分号可选，但分号之后不允许再出现第二条语句。
        match_symbol(";");
        if (peek().kind != TokenKind::end) {
            throw std::runtime_error("语句末尾存在多余内容: " + peek().text);
        }
    }

    const Token& peek() const { return tokens_[position_]; }
    const Token& previous() const { return tokens_[position_ - 1]; }
    Token advance() { return tokens_[position_++]; }

    bool match_keyword(std::string_view keyword) {
        if (peek().kind == TokenKind::word && equals_keyword(peek().text, keyword)) {
            ++position_;
            return true;
        }
        return false;
    }

    bool previous_keyword(std::string_view keyword) const {
        return previous().kind == TokenKind::word && equals_keyword(previous().text, keyword);
    }

    void expect_keyword(std::string_view keyword) {
        if (!match_keyword(keyword)) {
            throw std::runtime_error("期望关键字 " + std::string(keyword));
        }
    }

    bool match_symbol(std::string_view symbol) {
        if (peek().kind == TokenKind::symbol && peek().text == symbol) {
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

    std::vector<Token> tokens_;
    std::size_t position_{0};
};

}  // namespace

ParseResult parse_sql(const std::string& sql) {
    try {
        // 解析分为 lexer 和 parser 两阶段，异常统一转成调用方可处理的 ParseResult。
        Lexer lexer(sql);
        auto tokens = lexer.scan();
        if (tokens.size() == 1) {
            return ParseResult{std::nullopt, "SQL 不能为空"};
        }
        return ParseResult{Parser(std::move(tokens)).parse(), {}};
    } catch (const std::exception& exception) {
        return ParseResult{std::nullopt, exception.what()};
    }
}

}  // namespace minidb
