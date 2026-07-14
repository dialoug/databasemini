#include "minidb/common/sql_parser.hpp"
#include "minidb/mysql/mysql_database.hpp"
#include "minidb/postgres/postgres_database.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& description) {
    if (!condition) {
        ++failures;
        std::cerr << "[FAIL] " << description << '\n';
    } else {
        std::cout << "[ OK ] " << description << '\n';
    }
}

void parser_tests() {
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

void postgres_tests() {
    minidb::postgres::PostgresDatabase database;
    auto session = database.connect();

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

    session->execute("BEGIN;");
    session->execute("CREATE TABLE transactional_ddl (id INT);");
    session->execute("ROLLBACK;");
    expect(!session->execute("SELECT * FROM transactional_ddl;").ok,
           "PostgreSQL CREATE TABLE is transactional");

    session->execute("BEGIN;");
    expect(!session->execute("INSERT INTO accounts VALUES ('wrong', 'type');").ok,
           "PostgreSQL detects a statement error");
    expect(!session->execute("SELECT * FROM accounts;").ok,
           "PostgreSQL failed transaction rejects later statements");
    expect(session->execute("ROLLBACK;").ok,
           "PostgreSQL failed transaction can be rolled back");

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

    const auto engines = session->execute("SHOW ENGINES;");
    expect(engines.ok && engines.rows.size() == 2,
           "MySQL lists pluggable InnoDB and MEMORY engines");

    session->execute("CREATE TABLE accounts (id INT, owner TEXT) ENGINE=InnoDB;");
    session->execute("BEGIN;");
    session->execute("INSERT INTO accounts VALUES (1, 'alice');");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM accounts;").rows.empty(),
           "InnoDB rolls back transactional writes");

    session->execute("CREATE TABLE cache (id INT) ENGINE=MEMORY;");
    session->execute("BEGIN;");
    session->execute("INSERT INTO cache VALUES (5);");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM cache;").rows.size() == 1,
           "MEMORY engine keeps non-transactional writes after ROLLBACK");

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

    session->execute("BEGIN;");
    session->execute("INSERT INTO accounts VALUES (2, 'bob');");
    const auto ddl = session->execute("CREATE TABLE ddl_boundary (id INT);");
    expect(ddl.ok && !session->in_transaction(), "MySQL DDL implicitly commits the transaction");
    session->execute("ROLLBACK;");
    expect(session->execute("SELECT * FROM accounts;").rows.size() == 1,
           "MySQL implicit DDL commit preserves the earlier InnoDB write");

    session->execute("BEGIN;");
    expect(!session->execute("INSERT INTO accounts VALUES ('bad', 'row');").ok,
           "MySQL reports a statement error");
    expect(session->execute("INSERT INTO accounts VALUES (3, 'carol');").ok,
           "MySQL statement error does not poison the whole transaction");
    session->execute("COMMIT;");
}

}  // namespace

int main() {
    parser_tests();
    postgres_tests();
    mysql_tests();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
