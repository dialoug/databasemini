# PostgreSQL 风格与 MySQL 风格架构对照

## 总览

```text
pg_mini                                  mysql_mini
┌──────────────────────┐                 ┌──────────────────────────┐
│ logical postmaster   │                 │ logical mysqld           │
│ PostgresDatabase     │                 │ MysqlDatabase            │
└──────────┬───────────┘                 └────────────┬─────────────┘
           │ connect                                   │ connect
   ┌───────▼────────┐                         ┌────────▼─────────┐
   │ backend session │                         │ connection/thread │
   │ independent xid │                         │ SQL Server layer  │
   └───────┬────────┘                         └────────┬─────────┘
           │                                           │ handler API
   ┌───────▼────────┐                         ┌────────▼──────────┐
   │ heap versions  │                         │ StorageEngine      │
   │ xmin + status  │                         ├─────────┬──────────┤
   │ statement view │                         │ InnoDB  │ MEMORY   │
   └────────────────┘                         └─────────┴──────────┘
```

共享的 parser 只负责把本项目支持的 SQL 子集转成 AST。AST 进入两个数据库后，catalog、事务、可见性判断和存储路径全部分开。

## 行为对照

| 主题 | `pg_mini` | `mysql_mini` |
|---|---|---|
| 连接模型 | `PostgresSession` 表示每连接 backend process | `MysqlSession` 表示 mysqld 中的 connection/thread |
| 表存储 | 内建 heap table | Server catalog 将表路由到 `StorageEngine` |
| 默认引擎 | 固定 heap/MVCC 路径 | `InnoDB`，也可显式选择 `MEMORY` |
| 一致性读 | 默认 `READ COMMITTED`，每条 `SELECT` 创建新快照 | InnoDB 默认 `REPEATABLE READ`，事务第一次一致性读后复用 read view |
| 回滚 | aborted tuple 保留但不可见，`VACUUM` 清理 | InnoDB 版本变为 rolled back；MEMORY 写入不参与回滚 |
| 语句错误 | 整个事务进入 failed 状态，只接受 `ROLLBACK` | 当前语句失败，事务仍可继续 |
| DDL | `CREATE TABLE` 是事务性的 | `CREATE TABLE` 触发隐式提交 |
| 维护 | 显式 `VACUUM` | 维护策略属于各 engine；本模型未实现 purge |

## PostgreSQL 风格代码路径

入口位于 `src/postgres/postgres_database.cpp`。

1. `PostgresDatabase::connect()` 分配逻辑 backend PID。
2. `BEGIN` 分配 xid，并在共享事务状态表登记为 `in_progress`。
3. `INSERT` 向 heap 追加带 `xmin` 的 `TupleVersion`，不覆盖旧数据。
4. `SELECT` 为当前语句捕获活动 xid 集合，根据 `xmin`、提交状态和快照边界判断可见性。
5. `ROLLBACK` 把 xid 标为 `aborted`；版本继续占空间但不可见。
6. `VACUUM` 删除 aborted 版本，演示 PostgreSQL dead tuple 回收思想。

真实 PostgreSQL 还有 shared buffers、磁盘 page、WAL、CLOG/pg_xact、锁、HOT update、autovacuum 等。本项目只保留解释 MVCC 和进程边界所需的最短路径。

## MySQL 风格代码路径

入口位于 `src/mysql/mysql_database.cpp`，引擎位于 `src/mysql/*_engine.cpp`。

1. `MysqlDatabase::connect()` 分配 connection id，代表服务器进程中的一个连接执行上下文。
2. Server 层持有 schema 与“表 -> 引擎”映射，校验行后通过 `StorageEngine` 接口路由。
3. InnoDB 记录行版本和事务状态。事务第一次 `SELECT` 创建 `ReadView`，后续一致性读复用它。
4. MEMORY 直接保存行，忽略 begin/commit/rollback，因此可直观看到同一个 SQL 层下引擎能力不同。
5. `CREATE TABLE` 先提交当前事务，体现 MySQL DDL 隐式事务边界。

真实 MySQL/InnoDB 还有 buffer pool、redo/undo log、doublewrite、purge、change buffer、索引组织表、metadata lock 等。本项目的 InnoDB 是教学模拟，不是文件格式兼容实现。

## 两会话隔离级别实验

测试程序中已经自动执行这个实验：

```text
会话 A BEGIN
会话 A SELECT -> 0 行，建立视图
会话 B INSERT -> COMMIT
会话 A 再次 SELECT
```

- PostgreSQL 风格：第二次 `SELECT` 捕获新语句快照，可看到 B 的行。
- InnoDB 风格：第二次 `SELECT` 复用第一次 read view，仍看不到 B 的行；A 提交后再查询才可见。

这是两个产品默认隔离语义最适合用小程序观察的差异之一。

## 推荐练习顺序

1. 为 parser 增加 `DELETE FROM t WHERE id = ...`。
2. PostgreSQL heap version 增加 `xmax`；InnoDB 增加 undo chain 概念。
3. 加入表级锁，再细化到行锁和死锁检测。
4. 实现固定大小 page、buffer pool 与脏页刷盘。
5. 分别加入 PostgreSQL 风格 WAL 和 InnoDB 风格 redo/undo。
6. 加入 B+Tree 后比较 heap table 与 InnoDB clustered primary key 的访问路径。
