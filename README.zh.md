# uniorm

[English](README.md)

基于 ODBC 的现代 C++20 数据库访问层。不依赖特定厂商的 C 客户端，通过统一的
ODBC 接口访问任意提供 ODBC 驱动的数据库，在通用层之上提供实体映射、成员指
针查询构建、事务与连接池。

详细设计见 [docs/design.md](docs/design.md)。

## 特性

- **同步 API + 异常错误体系**：所有失败以异常抛出（`uniorm_error` 派生树）
- **内部统一 UTF-8**：转换仅发生在 ODBC 边界（UTF-16）
- **预编译 + 绑定变量**：用户值一律经 `SQLBindParameter`，杜绝拼接注入
- **透明的语句缓存**：按 SQL 文本的 LRU 缓存，重复执行免 prepare
  （观测：`statement_cache_hits()/misses()/size()`）
- **三种使用层次**：
  - 裸 SQL：`execute` / `execute_update` + `params`
  - 聚合投影：`conn.query<Row>(sql)` 零注册按列序映射到 struct
  - 实体映射：注册表 + 类型安全的成员指针谓词构建器
    `conn.query(orm).of<T>()`
- **实体直接绑定**：`query<T>::all()/one()` 将结果列直接绑到实体字段
  （`SQLBindCol`），跳过行物化
- **批量插入**：`conn.insert(orm, rows)` / `conn.insert_batch(...)`，
  多行 VALUES、自动分批、事务包裹
- **RAII 事务**：析构自动回滚
- **连接池**：懒创建、借还超时；全局单线程维护线程执行心跳保活与
  空闲超时回收
- **方言自适应**：标识符引号与分页语法按 `SQL_DBMS_NAME` 推断
  （MySQL/MariaDB 用反引号 + LIMIT/OFFSET，其余 ANSI）
- **代码生成**：`uniorm-gen` 连活库经 ODBC 元数据提取 schema，生成实体
  struct + 注册函数（TOML 覆写类型/类名/跳过表）
- **可插拔 backend**：核心 API 构建在驱动中立的 backend 接口之上，连接串
  scheme 选择后端（`odbc://...`；裸 ODBC 连接串保持向后兼容），能力缺失
  时明确抛错而非静默降级

## 要求

- C++20 编译器（GCC 11+ / Clang 14+）
- CMake ≥ 3.20
- unixODBC（`find_package(ODBC)`）及目标数据库的 ODBC 驱动

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

常用开关：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | Debug / Release / RelWithDebInfo / MinSizeRel |
| `UNIORM_DECIMAL_DEFAULT` | `string` | DECIMAL/NUMERIC 的默认 C++ 映射（`string` 或 `double`） |
| `UNIORM_BUILD_TESTS` | `ON` | 构建单元/集成/性能测试 |
| `UNIORM_BUILD_TOOLS` | `ON` | 构建工具（`uniorm-gen`） |
| `UNIORM_BACKEND_ODBC` | `ON` | 将 ODBC backend 编入 `libuniorm`；关闭后为纯核心构建（须同时关闭 `UNIORM_BUILD_TOOLS`） |

产物为动态库 `libuniorm.so`（头文件在 `include/uniorm/`）。

## 快速上手

### 连接与裸 SQL

```cpp
#include <uniorm/connection.hpp>

uniorm::connection conn("DSN=mydb;UID=user;PWD=secret");
// 等价的显式写法："odbc://DSN=mydb;UID=user;PWD=secret"。
// :// 之前的 scheme 选择 backend；无 scheme 的串按 ODBC 连接串处理。

auto rs = conn.execute("SELECT id, name FROM users WHERE age > ?",
                       uniorm::params{18});
while (rs.next()) {
    uniorm::row r = rs.current();
    // r.get<std::int64_t>(0), r.get<std::string>("name")
}

std::size_t n = conn.execute_update(
    "UPDATE users SET name = ? WHERE id = ?",
    uniorm::params{"alice", std::int64_t{1}});
```

### 聚合投影（零注册）

```cpp
struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

auto rows = conn.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", uniorm::params{18});
```

### 实体映射 + 查询构建器

```cpp
#include <uniorm/mapping/registry.hpp>
#include <uniorm/query/builder.hpp>

struct User {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

uniorm::orm registry;
registry.map<User>("users")
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age);

registry.validate(conn);  // 与数据库 schema 对账（可选）

using namespace uniorm;
auto adults = conn.query(registry)
                  .of<User>()
                  .where(gt(&User::age, 18) && like(&User::name, "a%"))
                  .order_by(&User::id, direction::desc)
                  .limit(10)
                  .all();  // 直接绑定到 User 字段
```

### 批量插入

```cpp
std::vector<User> users = /* ... */;
std::size_t n = conn.insert(registry, users);  // NULL、分批、事务全自动

// 无实体映射的动态版本
conn.insert_batch("users", {"name", "age"},
                  {uniorm::params{"alice", 30}, uniorm::params{"bob", nullptr}});
```

### 事务与连接池

```cpp
{
    auto txn = conn.begin();
    conn.execute_update("INSERT INTO logs (msg) VALUES (?)",
                        uniorm::params{"x"});
    txn.commit();  // 不 commit 则析构时回滚
}

uniorm::pool_options opts;
opts.connection_string = "DSN=mydb;UID=user;PWD=secret";
opts.size = 8;
uniorm::connection_pool pool(std::move(opts));

{
    auto c = pool.acquire();      // 超时抛 pool_timeout
    c->execute_update("...");
}  // 析构自动归还
```

连接池自带全局单线程维护：周期性对空闲连接执行心跳（默认 `SELECT 1`），
心跳失败即丢弃；空闲超过 `max_idle_time`（默认 10 分钟）的连接彻底释放。

### 代码生成（uniorm-gen）

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
```

输出 `build/gen/<name>_schema.hpp`：每表一个 struct（可空列自动
`std::optional`），PK/FK/索引信息以注释输出，并生成
`register_<name>_schema(uniorm::orm&)` 注册函数；`<name>` 默认取数据库名。
TOML 配置支持全局/单列类型覆写、类名覆写与跳表：

```toml
[types]                        # 全局 SQL 类型 → C++ 类型覆写
"NUMERIC(10,2)" = "std::int64_t"

[tables.t_user]
class = "User"                 # 类名覆写
skip = false

[tables.t_user.columns.status]
cpp_type = "std::string"       # 单列类型覆写
```

覆写仅限注册表可绑定的类型，完整规格见设计文档 §6。

## 测试

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**：纯内存测试，无外部依赖；基于 fake backend 运行且不链接
  ODBC，公共 API 一旦泄漏驱动类型即编译失败
- **odbc_unit_tests**：ODBC 句柄与 `uniorm-gen` 单元测试
  （仅驱动管理器，无需 DSN）
- **integration_tests**：需要可达的 ODBC DSN，读取环境变量
  `UNIORM_IT_DSN`、`UNIORM_IT_USER`、`UNIORM_IT_PWD`（三者均须设置），
  凭据以 `UID`/`PWD` 拼入连接串；任一未设置或连不上时以 ctest SKIP 处理
- **perf_tests**（ctest 标签 `perf`）：批量插入吞吐与三条查询物化路径
  （实体直绑 / 聚合投影 / 动态行）的对比，并附调用形式与 uniorm
  一一对应的纯 ODBC 基线作为抽象开销参照；行数由 `UNIORM_PERF_ROWS` 指定
  （默认 10000），可用 `ctest -LE perf` 跳过
- **gen_e2e_tests**：`uniorm-gen` 端到端——生成物与检入的 golden 头文件
  逐字节比对，golden 本身经编译、注册并 `validate(strict)`；需可达 DSN

## 目录结构

```
include/uniorm/       公共头文件
  backend/            驱动中立的 backend 接口、注册表、错误体系
  odbc/               ODBC 句柄 RAII 封装（environment/connection/statement）
  detail/             pfr-lite、投影绑定、语句缓存、chrono 工具
  mapping/            实体映射注册表
  query/              谓词表达式与查询构建器
src/                  实现（构建为 libuniorm.so）
  backend/            scheme 解析与 backend 注册表
  odbc/               ODBC backend（适配器、句柄封装、错误）
tools/uniorm-gen      代码生成 CLI（schema 提取 + TOML 配置 + 生成器）
tests/unit            单元测试
tests/integration     数据库集成测试（含 uniorm-gen 的 golden 头文件）
tests/perf            性能基准测试
docs/design.md        设计文档（权威 API 参考）
```

## 状态

v1 已完成并通过 MariaDB 集成验证（含 `uniorm-gen` 端到端）。v2 进行中：
backend 抽象已落地（中立接口 + scheme 注册表，ODBC 迁移至接口之后、
改为 PRIVATE 链接，核心单测在不链接 ODBC 的情况下编译运行）；后续为
libpq / Oracle OCI 原生 backend、数组绑定批量操作等，见设计文档 §5 与 §9。
