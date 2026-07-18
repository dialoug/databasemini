# PostgreSQL、MySQL、MongoDB、Redis 与 Neo4j 架构对照

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

redis_mini
┌──────────────────────────┐
│ logical redis-server     │
│ RedisDatabase            │
└────────────┬─────────────┘
             │ connect
     ┌───────▼──────────┐
     │ RedisSession     │
     │ MULTI queue      │
     └───────┬──────────┘
             │ direct hash lookup
     ┌───────▼──────────┐
     │ key -> string    │
     │ unordered_map    │
     └──────────────────┘

neo4j_mini
┌──────────────────────────┐
│ logical graph server     │
│ Neo4jDatabase            │
└────────────┬─────────────┘
             │ connect
     ┌───────▼──────────┐
     │ Neo4jSession     │
     │ graph snapshot   │
     └───────┬──────────┘
             │ Cypher pattern match
     ┌───────▼──────────┐
     │ labeled nodes    │
     │ typed relations  │
     └──────────────────┘
```

PostgreSQL 与 MySQL 共享的 parser 只负责把 SQL 子集转成 AST。AST 进入两个数据库后，catalog、事务、可见性判断和存储路径全部分开。MongoDB 使用独立命令 parser 和递归 BSON 类型；Redis 实现解析 `SET/GET/INCR/MULTI`，直接访问 hash keyspace；Neo4j 实现解析节点—关系模式，并沿有向边完成 `MATCH`。不同数据模型不会被强行塞进同一个 SQL AST。

## 行为对照

| 主题 | `pg_mini` | `mysql_mini` | `mongo_mini` | `redis_mini` | `neo4j_mini` |
|---|---|---|---|---|---|
| 连接模型 | backend process | connection/thread | driver session | Redis client connection | Neo4j driver session |
| 数据模型 | 固定 schema 的 heap row | 固定 schema 的表 | 灵活 collection/document | `key -> string value` | 属性节点与有向关系 |
| 存储路径 | heap/MVCC | `InnoDB` 或 `MEMORY` | 递归 BSON 与文档版本 | `unordered_map` 直接映射 | node/relationship graph state |
| 结构 | 固定列 | 固定列 | 嵌套对象和数组 | 不透明 string value | 标签、关系类型、标量属性 |
| 读取 | 每条 `SELECT` 新快照 | InnoDB 复用 read view | 教学版事务 snapshot | `GET key` 立即读取 | Cypher 模式遍历关系 |
| 事务/队列 | `BEGIN` 后立即执行 | `BEGIN` 后立即执行 | `BEGIN` 后立即执行 | `MULTI` 排队，`EXEC` 执行 | 图快照上立即执行 |
| 撤销 | `ROLLBACK` + `VACUUM` | InnoDB 回滚；MEMORY 保留 | rolled-back 版本不可见 | `DISCARD` 队列；执行后不回滚 | 丢弃事务图快照 |
| 数据定义 | 事务型 `CREATE TABLE` | DDL 隐式提交 | collection 无列定义 | 没有 catalog/schema | `CREATE` 同时创建图结构 |
| 查询入口 | `SELECT * FROM table` | `SELECT * FROM table` | `find()` 过滤文档 | `GET key` hash lookup | `MATCH (a)-[r]->(b)` |

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

真实 MongoDB 还有完整 BSON 类型、WiredTiger、replica set、read/write concern、aggregation pipeline、schema validation、change stream、索引、sharding 和真实多文档事务协调。本项目只保留文档模型与关系模型对照所需的最短路径。`BEGIN/COMMIT/ROLLBACK` 是为关系型/文档事务对照提供的教学别名，并非 `mongosh` 原生命令形式。

## Redis 风格代码路径

入口位于 `src/redis/redis_database.cpp`，命令解析位于 `src/redis/redis_parser.cpp`。

1. `RedisDatabase::connect()` 分配逻辑 connection id，每个 `RedisSession` 拥有独立的 MULTI 队列。
2. `SET key value` 直接写入 `unordered_map<string, string>`；没有表、collection、schema 或 planner。
3. `GET/DEL/EXISTS` 通过 key 执行一次 hash lookup；`KEYS` 是为了观察教学状态而保留的全 keyspace 扫描命令。
4. `PING` 验证命令路径，`TYPE` 返回 `string/none`，`DBSIZE` 返回当前 key 数量。
5. `INCR` 在服务器互斥锁内完成“读取整数、加一、写回”，展示服务端原子操作。
6. `MULTI` 之后的数据命令只返回 `QUEUED`；`EXEC` 一次持锁执行完整队列，因此其他连接看不到中间状态。
7. `DISCARD` 可以丢弃未执行队列，但 `EXEC` 中的运行时错误不会回滚已经执行或后续可继续执行的命令。

该实现明确模拟 Redis string keyspace 与事务队列，但不实现真实 Redis 的 RESP 协议、事件循环、TTL、RDB/AOF、replication、cluster、pub/sub、Lua，以及 list/set/hash/sorted set 等数据结构。所有 value 都只是字符串。

## Neo4j 风格代码路径

入口位于 `src/neo4j/neo4j_database.cpp`，Cypher 解析位于 `src/neo4j/cypher_parser.cpp`。

1. `Neo4jDatabase::connect()` 分配逻辑 driver session id。
2. 独立 parser 把 `(n:Person {name: 'Alice'})` 转换为节点模式，把 `-[r:KNOWS]->` 转换为有方向、有类型的关系模式。
3. `CREATE` 生成带内部 id、标签和标量属性的节点；路径形式还会生成起点 id、终点 id、类型和属性组成的关系。
4. 单节点 `MATCH` 按标签和内联属性过滤节点；单跳 `MATCH` 先扫描关系，再按起点、终点模式过滤，方向相反不会命中。
5. `RETURN n` 渲染完整节点或关系，`RETURN n.name` 只投影指定属性；缺失属性显示为 `null`。
6. 普通命令自动提交；`BEGIN` 为 session 复制图快照，自己的写入立即可见但其他 session 不可见，`ROLLBACK` 丢弃快照。
7. 写事务 `COMMIT` 原子发布完整图；若开始事务后已有其他写入提交，教学实现报告冲突并回滚，以避免覆盖并发更新。

真实 Neo4j 还支持完整 Cypher/GQL、多跳和可变长度路径、`WHERE/WITH/UNWIND`、索引与约束、查询计划、存储页、锁与死锁处理、procedure、集群和因果一致性。本项目只保留“关系是一等数据”和图模式遍历所需的最短路径。

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
- Neo4j 教学风格：事务在独立图快照上读写；自己的新节点可见，其他 session 在提交前不可见。

这组实验同时展示了“语句级视图”和“事务级视图”，但 MongoDB 与 Neo4j 部分都是教学简化，不能替代真实部署中的事务和集群行为验证。

Redis 模型不加入这组快照实验：普通命令立即生效，而 `MULTI` 队列在 `EXEC` 前根本尚未执行。它对照的是“版本可见性事务”和“原子命令批次”之间的差异。

## 推荐练习顺序

1. 为 SQL parser 增加 `DELETE FROM t WHERE id = ...`，为 Mongo parser 增加 `deleteOne({_id: ...})`。
2. PostgreSQL heap version 增加 `xmax`；InnoDB 增加 undo chain；MongoDB 增加 `updateOne` 文档版本。
3. 为 MongoDB 过滤器增加点路径、比较操作符和数组匹配，再实现简化 aggregation pipeline。
4. 为 Redis 模型加入 TTL、过期字典，以及 list/set/hash/sorted set 数据类型。
5. 为 Neo4j parser 加入 `WHERE`、多跳/可变长度路径、`SET/DELETE`，再为标签和属性建立索引。
6. 加入表级锁，再细化到行锁、文档锁、图实体锁和死锁检测。
7. 实现固定大小 page、buffer pool 与脏页刷盘。
8. 分别加入 PostgreSQL 风格 WAL、InnoDB redo/undo、MongoDB oplog 和 Redis AOF 概念。
9. 加入 B+Tree 后比较 heap table、InnoDB clustered primary key、MongoDB `_id` 索引、hash keyspace 和图标签索引的访问路径。
