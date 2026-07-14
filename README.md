# Database Architecture Lab

这是一个用于学习数据库内部架构的 C++20 教学项目。它实现了两个**刻意保持小型**的内存数据库：

- `pg_mini`：PostgreSQL 风格，展示 postmaster/backend、heap tuple、MVCC、语句级快照、事务失败状态和 `VACUUM`。
- `mysql_mini`：MySQL 风格，展示单服务器进程/连接线程、Server SQL 层、handler 接口，以及可插拔的 `InnoDB`、`MEMORY` 存储引擎。

项目的重点是让相同 SQL 在两种架构下呈现不同的代码路径和事务行为，而不是兼容真实 PostgreSQL/MySQL 的完整 SQL、网络协议或磁盘格式。

## 快速开始

需要 CMake 3.20+ 和支持 C++20 的编译器。

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Windows + Visual Studio 多配置生成器下运行：

```powershell
.\build\Debug\pg_mini.exe
.\build\Debug\mysql_mini.exe
```

Ninja、Makefiles 等单配置生成器通常运行 `./build/pg_mini` 和 `./build/mysql_mini`。

## 立即观察差异

在 `pg_mini` 中：

```sql
SHOW ARCHITECTURE;
CREATE TABLE events (id INT, text TEXT);
BEGIN;
INSERT INTO events VALUES (1, 'pending');
INSERT INTO events VALUES ('bad', 'type error');
SELECT * FROM events; -- 事务已失败，必须 ROLLBACK
ROLLBACK;
VACUUM events;
```

在 `mysql_mini` 中：

```sql
SHOW ARCHITECTURE;
SHOW ENGINES;
CREATE TABLE durable_data (id INT, text TEXT) ENGINE=INNODB;
CREATE TABLE volatile_cache (id INT, text TEXT) ENGINE=MEMORY;
BEGIN;
INSERT INTO durable_data VALUES (1, 'rolled back');
INSERT INTO volatile_cache VALUES (1, 'not rolled back');
ROLLBACK;
SELECT * FROM durable_data;
SELECT * FROM volatile_cache;
```

更多逐项对照和源码导航见 [架构说明](docs/architecture.md)。可直接管道执行的命令示例位于 `examples/`。

## 支持的 SQL 子集

```sql
CREATE TABLE table_name (column INT, column TEXT) [ENGINE=INNODB|MEMORY];
INSERT INTO table_name VALUES (1, 'text');
SELECT * FROM table_name;
BEGIN; -- 或 START TRANSACTION
COMMIT;
ROLLBACK;
SHOW ARCHITECTURE;
SHOW ENGINES;
VACUUM [table_name];
HELP;
QUIT;
```

标识符在两个教学实现中都会转为小写。字符串使用单引号，内部单引号写作 `''`。

## 目录结构

```text
include/minidb/common/       共享的值类型、结果表格和最小 SQL parser
include/minidb/postgres/     PostgreSQL 风格公开接口
include/minidb/mysql/        MySQL Server 与 StorageEngine 接口
src/postgres/                backend、事务状态、heap/MVCC、VACUUM
src/mysql/                   SQL layer、InnoDB、MEMORY
apps/                        两个交互式命令行程序
tests/                       无第三方依赖的行为测试
examples/                    两套演示脚本
docs/                        架构对照文档
```

## Git 工作流

仓库默认分支为 `main`，构建产物已通过 `.gitignore` 排除。建议学习实验各用一个短分支：

```powershell
git switch -c experiment/add-delete
git add include src tests docs
git commit -m "feat: add MVCC delete markers"
```

适合继续实现的练习包括 `UPDATE/DELETE` 与 `xmax`、WAL/redo log、B+Tree、buffer pool、SQL optimizer 和并发锁管理。

## 边界

这是单进程、内存内、教学用途的模型，不可用于生产环境。它没有网络协议、身份认证、磁盘页、崩溃恢复、完整隔离级别、索引、优化器和大部分 SQL。逻辑 backend/connection 可从多个宿主线程调用，但实现用全局互斥锁保护教学状态，并不模拟真实系统的并行吞吐。
