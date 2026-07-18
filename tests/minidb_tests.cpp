#include "minidb/common/sql_parser.hpp"
#include "minidb/mongo/mongo_database.hpp"
#include "minidb/mongo/mongo_parser.hpp"
#include "minidb/mysql/mysql_database.hpp"
#include "minidb/postgres/postgres_database.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

// 极小断言工具保持测试零依赖，并在失败时继续运行后续场景以收集更多信息。
void expect(bool condition, const std::string& description) {
    if (!condition) {
        ++failures;
        std::cerr << "[FAIL] " << description << '\n';
    } else {
        std::cout << "[ OK ] " << description << '\n';
    }
}

void parser_tests() {
    // parser 测试关注 AST 生成和输入边界，不涉及任何数据库执行语义。
    const auto create = minidb::parse_sql(
        "CREATE TABLE Users (id INT, name TEXT) ENGINE=InnoDB;");
    expect(create.ok(), "parser accepts CREATE TABLE with an engine");
    expect(create.ok() && std::holds_alternative<minidb::CreateTableStatement>(*create.statement),
           "parser creates the correct AST node");

    const auto insert = minidb::parse_sql("INSERT INTO users VALUES (-7, 'O''Reilly');");
    expect(insert.ok(), "parser accepts negative integers and escaped quotes");

    const auto invalid = minidb::parse_sql("DELETE FROM users;");
    expect(!invalid.ok(), "parser rejects unsupported SQL");

    const auto with_bom = minidb::parse_sql("\xEF\xBB\xBFSHOW ARCHITECTURE;");
    expect(with_bom.ok(), "parser accepts an optional UTF-8 BOM from Windows pipes");
}

void mongo_parser_tests() {
    // Mongo parser 接受递归文档，而不是把文档语法塞入关系型 SQL AST。
    const auto insert = minidb::mongo::parse_command(
        "db.users.insertOne({_id: 1, profile: {city: 'Shanghai'}, "
        "tags: ['vip', 'beta'], active: true});");
    expect(insert.ok(), "Mongo parser accepts nested documents and arrays");
    expect(insert.ok() &&
               std::holds_alternative<minidb::mongo::InsertOneStatement>(
                   *insert.statement),
           "Mongo parser creates an insertOne AST node");

    const auto find = minidb::mongo::parse_command("db.users.find({name: 'Alice'});");
    expect(find.ok(), "Mongo parser accepts a top-level equality filter");

    const auto invalid = minidb::mongo::parse_command("db.users.deleteMany({});");
    expect(!invalid.ok(), "Mongo parser rejects unsupported collection methods");
}

void postgres_tests() {
    minidb::postgres::PostgresDatabase database;
    auto session = database.connect();

    // 基本 MVCC：事务能看到自己的写入，回滚后其他快照看不到该 tuple。
    expect(session->execute("CREATE TABLE accounts (id INT, owner TEXT);").ok,
           "PostgreSQL creates a heap table");
    expect(session->execute("BEGIN;").ok, "PostgreSQL begins a transaction");
    expect(session->execute("INSERT INTO accounts VALUES (1, 'alice');").ok,
           "PostgreSQL inserts an MVCC tuple");
    expect(session->execute("SELECT * FROM accounts;").rows.size() == 1,
           "PostgreSQL transaction sees its own insert");
    expect(session->execute("ROLLBACK;").ok, "PostgreSQL rolls back");
    expect(session->execute("SELECT * FROM accounts;").rows.empty(),
           "PostgreSQL hides a rolled-back tuple");

    // PostgreSQL 的 CREATE TABLE 可以和普通数据修改一起参与事务回滚。
    session->execute("BEGIN;");
    session->execute("CREATE TABLE transactional_ddl (id INT);");
    session->execute("ROLLBACK;");
    expect(!session->execute("SELECT * FROM transactional_ddl;").ok,
           "PostgreSQL CREATE TABLE is transactional");

    // PostgreSQL 事务中的任意语句错误会阻止后续语句，直到显式回滚。
    session->execute("BEGIN;");
    expect(!session->execute("INSERT INTO accounts VALUES ('wrong', 'type');").ok,
           "PostgreSQL detects a statement error");
    expect(!session->execute("SELECT * FROM accounts;").ok,
           "PostgreSQL failed transaction rejects later statements");
    expect(session->execute("ROLLBACK;").ok,
           "PostgreSQL failed transaction can be rolled back");

    // 两会话实验：READ COMMITTED 的第二条 SELECT 使用新快照并看到 writer 的提交。
    session->execute("CREATE TABLE events (id INT);");
    auto reader = database.connect();
    auto writer = database.connect();
    reader->execute("BEGIN;");
    expect(reader->execute("SELECT * FROM events;").rows.empty(),
           "PostgreSQL first READ COMMITTED statement sees no rows");
    writer->execute("INSERT INTO events VALUES (10);");
    expect(reader->execute("SELECT * FROM events;").rows.size() == 1,
           "PostgreSQL READ COMMITTED refreshes the snapshot per statement");
    reader->execute("ROLLBACK;");

    expect(session->execute("VACUUM;").ok, "PostgreSQL exposes tuple cleanup via VACUUM");
}

void mysql_tests() {
    minidb::mysql::MysqlDatabase database;
    auto session = database.connect();

    // Server 层能够列出并通过统一接口管理事务型和非事务型引擎。
    const auto engines = session->execute("SHOW ENGINES;");
    expect(engines.ok && engines.rows.size() == 2,
           "MySQL lists pluggable InnoDB and MEMORY engines");

    // InnoDB 接收事务回调，ROLLBACK 后版本不可见。
    session->execute("CREATE TABLE accounts (id INT, owner TEXT) ENGINE=InnoDB;");
    session->execute("BEGIN;");
    session->execute("INSERT INTO accounts VALUES (1, 'alice');");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM accounts;").rows.empty(),
           "InnoDB rolls back transactional writes");

    // MEMORY 忽略事务回调，写入在 ROLLBACK 后仍然存在。
    session->execute("CREATE TABLE cache (id INT) ENGINE=MEMORY;");
    session->execute("BEGIN;");
    session->execute("INSERT INTO cache VALUES (5);");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM cache;").rows.size() == 1,
           "MEMORY engine keeps non-transactional writes after ROLLBACK");

    // 两会话实验：REPEATABLE READ 复用第一次 ReadView，不看到 writer 的新提交。
    session->execute("CREATE TABLE events (id INT) ENGINE=InnoDB;");
    auto reader = database.connect();
    auto writer = database.connect();
    reader->execute("BEGIN;");
    expect(reader->execute("SELECT * FROM events;").rows.empty(),
           "InnoDB first consistent read sees no rows");
    writer->execute("INSERT INTO events VALUES (10);");
    expect(reader->execute("SELECT * FROM events;").rows.empty(),
           "InnoDB REPEATABLE READ reuses its transaction read view");
    reader->execute("COMMIT;");
    expect(reader->execute("SELECT * FROM events;").rows.size() == 1,
           "InnoDB sees the committed row in a new transaction");

    // DDL 会先隐式提交当前事务，因此随后的 ROLLBACK 无法撤销此前 INSERT。
    session->execute("BEGIN;");
    session->execute("INSERT INTO accounts VALUES (2, 'bob');");
    const auto ddl = session->execute("CREATE TABLE ddl_boundary (id INT);");
    expect(ddl.ok && !session->in_transaction(), "MySQL DDL implicitly commits the transaction");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM accounts;").rows.size() == 1,
           "MySQL implicit DDL commit preserves the earlier InnoDB write");

    // 和 PostgreSQL 对照：MySQL 中一条类型错误不会让整个事务进入失败状态。
    session->execute("BEGIN;");
    expect(!session->execute("INSERT INTO accounts VALUES ('bad', 'row');").ok,
           "MySQL reports a statement error");
    expect(session->execute("INSERT INTO accounts VALUES (3, 'carol');").ok,
           "MySQL statement error does not poison the whole transaction");
    session->execute("COMMIT;");
}

void mongo_tests() {
    minidb::mongo::MongoDatabase database;
    auto session = database.connect();

    expect(session->execute("db.createCollection('users');").ok,
           "MongoDB creates a collection without a column schema");
    expect(session->execute(
               "db.users.insertOne({_id: 1, name: 'Alice', "
               "profile: {city: 'Shanghai'}, tags: ['vip']});")
               .ok,
           "MongoDB inserts a nested BSON-style document");
    expect(session->execute(
               "db.users.insertOne({_id: 2, name: 'Bob', active: true});")
               .ok,
           "MongoDB accepts a different document shape in the same collection");
    expect(session->execute("db.users.find();").rows.size() == 2,
           "MongoDB find returns documents with flexible schemas");
    expect(session->execute("db.users.find({name: 'Alice'});").rows.size() == 1,
           "MongoDB find applies a top-level equality filter");
    expect(!session->execute("db.users.insertOne({_id: 1, name: 'duplicate'});").ok,
           "MongoDB enforces unique _id values");
    expect(session->execute("db.users.insertOne({name: 'auto-id'});").ok,
           "MongoDB generates an _id that avoids explicit identifiers");

    // 事务内文档对自己可见，对其他 session 不可见，回滚后保持不可见。
    session->execute("db.createCollection('events');");
    auto observer = database.connect();
    session->execute("BEGIN;");
    expect(session->execute("db.events.insertOne({kind: 'pending'});").ok,
           "MongoDB transaction inserts a document");
    expect(session->execute("db.events.find();").rows.size() == 1,
           "MongoDB transaction sees its own insert");
    expect(observer->execute("db.events.find();").rows.empty(),
           "MongoDB hides an uncommitted document from another session");
    session->execute("ROLLBACK;");
    expect(observer->execute("db.events.find();").rows.empty(),
           "MongoDB abort hides the rolled-back document");

    // 事务快照保持稳定；提交后的新查询才能看到其他 session 的提交。
    auto reader = database.connect();
    auto writer = database.connect();
    reader->execute("session.startTransaction();");
    expect(reader->execute("db.events.find();").rows.empty(),
           "MongoDB snapshot transaction initially sees no documents");
    writer->execute("db.events.insertOne({kind: 'committed'});");
    expect(reader->execute("db.events.find();").rows.empty(),
           "MongoDB transaction keeps its original snapshot");
    reader->execute("session.commitTransaction();");
    expect(reader->execute("db.events.find();").rows.size() == 1,
           "MongoDB sees the committed document after its transaction ends");

    // insertOne 可以隐式创建 collection；事务回滚同时撤销该教学 catalog 项。
    session->execute("BEGIN;");
    session->execute("db.ephemeral.insertOne({value: 1});");
    session->execute("ROLLBACK;");
    expect(!session->execute("db.ephemeral.find();").ok,
           "MongoDB rollback removes an implicitly created collection in this model");

    const auto architecture = session->execute("SHOW ARCHITECTURE;");
    expect(architecture.ok && !architecture.rows.empty(),
           "MongoDB exposes its document architecture for comparison");
}

}  // namespace

int main() {
    // 按层次执行：先验证 parser，再验证三套独立的执行/存储路径。
    parser_tests();
    mongo_parser_tests();
    postgres_tests();
    mysql_tests();
    mongo_tests();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
