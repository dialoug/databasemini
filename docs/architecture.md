# PostgreSQL、MySQL 与 MongoDB 风格架构对照

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

mongo_mini
┌──────────────────────────┐
│ logical mongod           │
│ MongoDatabase            │
└────────────┬─────────────┘
             │ connect
     ┌───────▼──────────┐
     │ driver session   │
     │ document commands│
     └───────┬──────────┘
             │ collection lookup
     ┌───────▼──────────┐
     │ flexible BSON    │
     │ document versions│
     └──────────────────┘
```

PostgreSQL 与 MySQL 共享的 parser 只负责把 SQL 子集转成 AST。AST 进入两个数据库后，catalog、事务、可见性判断和存储路径全部分开。MongoDB 使用独立命令 parser 和递归 BSON 类型，因为文档、数组、collection 操作并不适合伪装成关系型行和 SQL。

## 行为对照

| 主题 | `pg_mini` | `mysql_mini` | `mongo_mini` |
|---|---|---|---|
| 连接模型 | `PostgresSession` 表示每连接 backend process | `MysqlSession` 表示 mysqld 中的 connection/thread | `MongoSession` 表示驱动逻辑会话 |
| 数据模型 | 固定 schema 的 heap row | 固定 schema 的表，经 handler 路由 | 无固定列的 collection/document |
| 存储路径 | 固定 heap/MVCC 路径 | `InnoDB` 或 `MEMORY` | 递归 BSON 与文档版本 |
| 嵌套数据 | 当前 SQL 子集不支持 | 当前 SQL 子集不支持 | 原生对象和数组 |
| 一致性读 | `READ COMMITTED`，每条 `SELECT` 创建新快照 | InnoDB `REPEATABLE READ`，复用第一次 read view | 教学版事务从 `BEGIN` 起复用 snapshot |
| 回滚 | aborted tuple 保留但不可见，`VACUUM` 清理 | InnoDB 版本 rolled back；MEMORY 写入保留 | rolled-back 文档版本不可见 |
| 数据定义 | `CREATE TABLE` 事务性 | `CREATE TABLE` 隐式提交 | collection 无列定义，`insertOne` 可隐式创建 |
| 查询 | `SELECT * FROM table` | `SELECT * FROM table` | `find()` 与顶层字段精确过滤 |

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

## MongoDB 风格代码路径

入口位于 `src/mongo/mongo_database.cpp`，命令解析与 BSON 分别位于 `src/mongo/mongo_parser.cpp` 和 `src/mongo/bson.cpp`。

1. `MongoDatabase::connect()` 分配逻辑 driver session id。
2. 独立 parser 将 `db.users.insertOne({...})` 等命令转换成 Mongo AST；对象和数组递归转换为 `BsonValue`。
3. collection 只保存名称和文档版本，不持有统一列定义，因此同一 collection 的文档可以拥有不同字段。
4. `insertOne` 在缺少 `_id` 时生成教学版整数 id，并对未回滚版本检查唯一性；collection 不存在时可隐式创建。
5. `find()` 输出完整 JSON 文档；带过滤器时执行顶层字段精确匹配，不实现真实 MongoDB 的完整查询运算符。
6. `BEGIN` 或 `session.startTransaction()` 建立简化快照；自己的写入可见，其他 session 的未提交或快照之后写入不可见。

真实 MongoDB 还有完整 BSON 类型、WiredTiger、replica set、read/write concern、aggregation pipeline、schema validation、change stream、索引、sharding 和真实多文档事务协调。本项目只保留文档模型与关系模型对照所需的最短路径。`BEGIN/COMMIT/ROLLBACK` 是为三库对照提供的教学别名，并非 `mongosh` 原生命令形式。

## 多会话快照实验

测试程序中已经自动执行这个实验：

```text
会话 A BEGIN
会话 A SELECT -> 0 行，建立视图
会话 B INSERT -> COMMIT
会话 A 再次 SELECT
```

- PostgreSQL 风格：第二次 `SELECT` 捕获新语句快照，可看到 B 的行。
- InnoDB 风格：第二次 `SELECT` 复用第一次 read view，仍看不到 B 的行；A 提交后再查询才可见。
- MongoDB 教学风格：事务开始时固定 snapshot，第二次 `find()` 仍看不到 B 的文档；结束事务后新查询可见。

这组实验同时展示了“语句级视图”和“事务级视图”，但 MongoDB 部分是教学简化，不能替代真实部署中的 read concern、write concern 和 replica set 行为验证。

## 推荐练习顺序

1. 为 SQL parser 增加 `DELETE FROM t WHERE id = ...`，为 Mongo parser 增加 `deleteOne({_id: ...})`。
2. PostgreSQL heap version 增加 `xmax`；InnoDB 增加 undo chain；MongoDB 增加 `updateOne` 文档版本。
3. 为 MongoDB 过滤器增加点路径、比较操作符和数组匹配，再实现简化 aggregation pipeline。
4. 加入表级锁，再细化到行锁/文档锁和死锁检测。
5. 实现固定大小 page、buffer pool 与脏页刷盘。
6. 分别加入 PostgreSQL 风格 WAL、InnoDB 风格 redo/undo，并为 MongoDB 模型加入 oplog 概念。
7. 加入 B+Tree 后比较 heap table、InnoDB clustered primary key 和 MongoDB `_id` 索引的访问路径。
