# uniorm

基于 ODBC 的现代 C++20 数据库访问层。不依赖特定厂商的 C 客户端，通过统一的
ODBC 接口访问任意提供 ODBC 驱动的数据库（当前以 MariaDB 做集成验证），
在通用层之上提供实体映射、成员指针查询构建、事务与连接池。

详细设计见 [docs/design.md](docs/design.md)。

## 特性

- **同步 API + 异常错误体系**：所有失败以异常抛出（`uniorm_error` 派生树）
- **内部统一 UTF-8**：宽字符接口自动转换，驱动层处理 UTF-16
- **预编译 + 绑定变量**：用户值一律经 `SQLBindParameter`，杜绝拼接注入
- **语句缓存**：按 SQL 文本的透明 LRU 缓存，重复执行免 prepare
  （观测：`statement_cache_hits()/misses()`）
- **三种使用层次**：
  - 裸 SQL：`execute` / `execute_update` + `params`
  - 聚合投影：`conn.query<Row>(sql)` 零注册按列序映射到 struct
  - 实体映射：注册表 + 成员指针谓词构建器 `conn.query(orm).of<T>()`
- **批量插入**：`conn.insert(orm, rows)` / `conn.insert_batch(...)`，
  多行 VALUES、自动分批、事务包裹
- **RAII 事务**：析构自动回滚
- **连接池**：懒创建、借还超时；全局单线程心跳维护 + 空闲超时回收
- **方言自适应**：标识符引号与分页语法按 `SQL_DBMS_NAME` 推断
  （MySQL/MariaDB 用反引号 + LIMIT/OFFSET，其余 ANSI）

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
| `UNIORM_BUILD_TESTS` | `ON` | 构建单元/集成测试 |

产物为动态库 `libuniorm.so`（头文件在 `include/uniorm/`）。

## 快速上手

### 连接与裸 SQL

```cpp
#include <uniorm/connection.hpp>

uniorm::connection conn("DSN=docker_maria;UID=user;PWD=secret");

auto rs = conn.execute("SELECT id, name FROM users WHERE age > ?",
                       uniorm::params{18});
while (rs.next()) {
    uniorm::row r = rs.current();
    // r.get<std::int64_t>(0), r.get<std::string>(1)
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
                  .all();
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
opts.connection_string = "DSN=docker_maria;UID=user;PWD=secret";
opts.size = 8;
uniorm::connection_pool pool(std::move(opts));

{
    auto c = pool.acquire();      // 超时抛 pool_timeout
    c->execute_update("...");
}  // 析构自动归还
```

连接池自带全局单线程维护：周期性对空闲连接执行心跳（默认 `SELECT 1`），
心跳失败即丢弃；空闲超过 `max_idle_time`（默认 10 分钟）的连接彻底释放。

## 测试

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**：纯内存测试，无外部依赖
- **integration_tests**：需要可达的 ODBC DSN，默认读取环境变量
  `UNIORM_IT_DSN`（默认 `docker_maria`）、`UNIORM_IT_USER`、`UNIORM_IT_PWD`，
  凭据以 `UID`/`PWD` 拼入连接串；连不上时以 ctest SKIP 处理
- **perf_tests**（ctest 标签 `perf`）：批量插入与三条查询物化路径
  （实体直绑 / 聚合投影 / 动态行）的吞吐对比，并附调用形式与 uniorm
  一一对应的纯 ODBC 基线（多行 VALUES 批量插入、SQLBindCol 扫描、
  单行 LIMIT 1）作为抽象开销参照；行数由 `UNIORM_PERF_ROWS` 指定
  （默认 10000），可用 `ctest -LE perf` 跳过

## 目录结构

```
include/uniorm/       公共头文件
  odbc/               ODBC 句柄 RAII 封装（environment/connection/statement）
  detail/             pfr-lite、投影绑定、参数暂存、chrono 工具
  mapping/            实体映射注册表
  query/              谓词表达式与查询构建器
src/                  实现（构建为 libuniorm.so）
tests/unit            单元测试
tests/integration     数据库集成测试
tests/perf            性能基准测试
docs/design.md        设计文档（权威 API 参考）
```

## 状态

v1 已完成并通过 MariaDB 集成测试。v2 规划：backend 抽象（libpq / Oracle OCI
原生通道）、代码生成工具 `uniorm-gen`、数组绑定批量操作等，见设计文档 §9。
