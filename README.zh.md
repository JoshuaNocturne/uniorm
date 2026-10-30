# uniorm

[English](README.md)

现代 C++20 数据库访问层，核心是驱动中立的：连接串里的 scheme 选择 backend，
API 本身不点任何一家的客户端名。自带三家：默认构建的 ODBC，以及由构建
开关打开的 Oracle OCI 与 libpq。实体映射、成员指针查询构建、事务、连接池
与异步都建在这层核心之上。

权威 API 参考见 [docs/design.md](docs/design.md)。

## 特性

- **同步 API + 异常错误体系**：所有失败以 `uniorm_error` 派生树抛出
  （`column_not_found`/`type_mismatch`/`mapping_error`/`pool_timeout`）；
  backend 侧失败以 `backend::backend_error` 到达并携带原生诊断（ODBC 那家是
  SQLSTATE 与消息），backend 答不了的读取以
  `backend::capability_not_supported` 到达
- **内部统一 UTF-8**：字符串一律以窄字节递交（ODBC 那家是 `SQL_C_CHAR`）；
  与服务端编码的转换留在 backend 里，不在本库
- **预编译 + 绑定变量**：用户值一律走 backend 的绑定调用（ODBC 那家是
  `SQLBindParameter`），杜绝拼接注入
- **透明的语句缓存**：按 SQL 文本的 LRU 缓存，重复执行免 prepare（观测：
  `statement_cache_hits()`/`misses()`/`statement_cache_size()`，
  `clear_statement_cache()` 清空）
- **三种使用层次**：裸 SQL（`execute` / `execute_update` + `params`）、聚合投影
  （`db.query<Row>(sql)`，零注册）、实体映射（`db.query<T>()`）
- **实体直接绑定**：`query<T>::all()/one()` 将结果列直接绑到实体字段
  （ODBC 那家是 `SQLBindCol`），跳过行物化
- **自定义类型映射**：特化 `uniorm::converter<T>` 即命名域类型所绑定的 SQL
  表示；strict 模式的 `validate()` 会拿它与活库的列类型比对
- **精确定点小数值**：`uniorm::decimal_t` 存尾数 + scale，按值比较、逐值零分配；
  DECIMAL/NUMERIC 列默认生成为无损的 `std::string` 成员，按列 `cpp_type` 覆写即
  换成 `decimal_t`
- **批量写入**：`db.insert(rows)` 与批量 `db.update(rows)` /
  `db.remove(rows)`：一条占位符语句按参数组数组发出（ODBC 那家是
  `SQL_ATTR_PARAMSET_SIZE`），按 `paramset_size` 分批；连接处于 autocommit
  时整批扫描包进事务
- **RAII 事务**：析构自动回滚
- **可选的异步**（`async.hpp`）：同一个同步契约套上
  `uniorm::async::task<T>`：一把会话同时只跑一个操作，三种 drive 动作
  （`co_await`、`block()`、`start()`）；四个 CRUD 名字还各有一层收回调的重载
- **连接池**：懒创建、借还超时；所有池共用一条全局维护线程执行心跳保活与
  空闲超时回收
- **活库 schema 自省**：`orm::schema()` 与 `connection::schema()` 交出一个
  `uniorm::schema_meta`：库名、表清单，以及每张表的列、主键、外键与索引
- **方言自适应**：标识符引号与分页语法按服务端自报的家门推断（ODBC 那家是
  `SQL_DBMS_NAME`：MySQL/MariaDB 用反引号 + LIMIT/OFFSET，其余 ANSI）；
  标识符拼法另有一把部署级开关，`keep`（默认）/ `lower` / `upper`
- **代码生成**：`uniorm-gen` 连活库读取 schema，或读一份建表脚本，生成实体
  struct + 注册函数（TOML 覆写类型/类名/跳过表）
- **可插拔 backend**：连接串 scheme 选择后端（`odbc://...`、`oracle://...`、
  `postgresql://...`；裸 ODBC 连接串保持向后兼容）。另一种写法是
  `connection_spec`：scheme、target、user、password 四件，交出去时不拼词，怎么
  拼由那一家 backend 说了算。目录读取也是
  这份接口的一部分。

## 要求

- C++20 编译器——构建只要求 `cxx_std_20`，不编码任何版本下限
- CMake ≥ 3.21
- unixODBC（`find_package(ODBC)`）及目标数据库的 ODBC 驱动——仅在
  `UNIORM_BACKEND_ODBC=ON` 时需要
- Oracle Call Interface（Instant Client，或完整客户端的 SDK）——仅在
  `UNIORM_BACKEND_ORACLE_OCI=ON` 时需要；用 `OCI_ROOT` 或 `ORACLE_HOME`
  指到那棵树
- libpq（`find_package(PostgreSQL)`）——仅在 `UNIORM_BACKEND_LIBPQ=ON` 时需要

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

常用开关：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | Debug / Release / RelWithDebInfo / MinSizeRel |
| `UNIORM_BUILD_TESTS` | `ON` | 构建单元/集成/性能测试 |
| `UNIORM_BUILD_TOOLS` | `ON` | 构建工具（`uniorm-gen`） |
| `UNIORM_BACKEND_ODBC` | `ON` | 构建 ODBC backend 那一份 plugin（`libuniorm_odbc.so`）；关闭后只剩一个没有任何 backend 的核心库，此时 `UNIORM_BUILD_TOOLS=ON` 是配置错误 |
| `UNIORM_BACKEND_ORACLE_OCI` | `OFF` | 构建 Oracle OCI backend（目标 `uniorm_oracle_oci`，装出去仍叫 `libuniorm_oracle`）；配置它的那台机器上要有 Oracle Call Interface |
| `UNIORM_BACKEND_LIBPQ` | `OFF` | 构建 PostgreSQL backend（目标 `uniorm_libpq`，装出去仍叫 `libuniorm_postgresql`）；配置它的那台机器上要有 libpq |

产物为动态库 `libuniorm.so`（头文件在 `include/uniorm/`），`VERSION` 为
0.4.0，`SOVERSION` 取 major.minor（0.4）；每家启用的 backend 是自己的一份
`libuniorm_<scheme>.so`，核心库不链任何驱动。`SONAME` 与 plugin 的 ABI 门一起按
`major.minor` 走，minor 对不上的 plugin 装载时即被拒；消费者与第三方 backend
请与核心同批重新编译，不要混用二进制。

### 安装

```sh
cmake --install build --prefix /path/to/prefix
```

带版本号的动态库（含 `SOVERSION` 符号链接；Windows 下为 DLL 与导入库）进
`<libdir>/`，每家启用的 backend 进 `<libdir>/uniorm/backends/`（Windows 下是
`<bindir>/uniorm/backends/`），全部 public 头文件进 `<prefix>/include/uniorm/`，
包配置进 `<libdir>/cmake/uniorm/`，`uniorm-gen` CLI 进 `<bindir>/`。消费者不需要
本仓库的任何其他文件：

```cmake
find_package(uniorm REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE uniorm::uniorm)
```

C++20 的要求随目标一起导出，消费者无需自己再设 `CMAKE_CXX_STANDARD`。用哪家
backend 由运行时按 scheme 决定，从上面那个 `uniorm/backends/` 目录里取；
`UNIORM_PLUGIN_PATH` 改写 loader 去哪找。源码集成方式不变：`add_subdirectory`
或 FetchContent 同样拿到 `uniorm::uniorm` 目标，且不安装任何东西。安装规则只在
uniorm 是顶层工程时生效，CLI 只在 `UNIORM_BUILD_TOOLS=ON` 时安装。

## 快速上手

### 连接与裸 SQL

```cpp
#include <uniorm/uniorm.hpp>

uniorm::orm db("DSN=mydb;UID=user;PWD=secret");
// 等价的显式写法："odbc://DSN=mydb;UID=user;PWD=secret"。:// 之前的 scheme
// 选择 backend；无 scheme 的串按 ODBC 连接串处理，关掉 ODBC backend 的构建
// 会抛 unknown_scheme。
//
// 这个串同时是池的键：该构造器从一个以 DSN + UID 为键的进程级池里借连接，
// 用的是默认 pool_options。想自选容量、超时与维护线程，就传入你自己的
// connection_pool。

// 同一个库，不必写某一家的串：
uniorm::orm pieces(uniorm::connection_spec{
    "odbc",                       // uniorm 自己只读这一个字段
    "mydb",                       // 去处：DSN 名、host:port/db……
    "user", "secret"});           // 空 = 没有提供凭据

auto rs = db.execute("SELECT id, name FROM users WHERE age > ?",
                     uniorm::params{18});
while (rs.next()) {
    uniorm::row r = rs.current();
    // r.get<std::int64_t>(0), r.get<std::string>("name")
}

std::size_t n = db.execute_update(
    "UPDATE users SET name = ? WHERE id = ?",
    uniorm::params{"alice", std::int64_t{1}});
```

`connection_spec` 把四件套交给 scheme 所属的那家 backend，因为只有它知道这些
该怎么拼：ODBC 那家写成 `DSN=` / `UID=` / `PWD=`，该包花括号的值就包上。
`orm::connect(...)` 收同一个形状。它并不声称够用——backend 自己的关键字、端口、
TLS 策略这些都仍归串管，而串是每个 backend 都要实现的原语。同一个库，一份用
spec 建、一份用串建，是两个池。

### 聚合投影（零注册）

```cpp
struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

auto rows = db.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", uniorm::params{18});
```

### 实体映射 + 查询构建器

```cpp
struct User {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

db.map<User>("users")
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age);

db.validate();  // 与活库 schema 对账；默认 strict：表或列不在、成员绑不了
                // 该列的类型、可空列背后是非 optional 成员，都抛
// db.validate(uniorm::validation_mode::lenient);  // 只查存在性

using namespace uniorm;
auto adults = db.query<User>()
                .where(gt(&User::age, 18) && like(&User::name, "a%"))
                .order_by(&User::id, direction::desc)
                .limit(10)
                .all();  // 直接绑定到 User 字段
```

### Schema 自省

```cpp
uniorm::schema_meta& md = db.schema();  // backend 不提供自省即抛
                                        // capability_not_supported

for (auto const& tbl : md.tables("", "public")) {  // "" 表示不限定
    uniorm::schema_meta::table_ref ref{tbl.catalog, tbl.schema, tbl.name};
    for (auto const& c : md.table_columns(ref)) {
        // 映射校验读的就是 c.shape 那三样：name、type、nullable；另有
        // c.type_name、c.size、c.decimals、c.default_value，只有活库读取
        // 才带得回来
    }
    auto keys = md.primary_key(ref);      // 列名，按主键顺序
    auto fks  = md.foreign_keys(ref);     // 每一对列一行
    auto idx  = md.indexes(ref);          // 名字、列、是否唯一
}

// validate() 唯一的那次读取，约成被校验的那个子集：
uniorm::table_shape shape = md.shape({{}, {}, "users"});
if (auto const* id = uniorm::find_column(shape, "id")) {
    bool nullable = id->nullable;
}
```

`table_ref` 的 catalog 与 schema 留空，等于在连接看得见的所有地方找这个名字，
于是同一张表名跨两个 schema 会被约成一个合并后的 shape；需要区分时就在 `ref`
里写明 schema。这个引用属于所借的那条连接，别跨 `disconnect()` 或重新借出缓存
它。

### 标识符拼法

```cpp
db.identifier_case(uniorm::dialect::identifier_case::upper);  // 默认不声明
```

不声明时跟着服务器自己那家方言走：Oracle 把不加引号的名字折成大写，PostgreSQL
折成小写，这两家什么都不用声明；其余家原样发出声明的名字——把 `keep` 声明出来
是一个决定，不是没有决定。`lower` 与 `upper` 只折 ASCII 字母。连接之后声明一次
即可，`orm` 每次租到连接都会重贴上去。折叠在哪条腿上够着这家服务器就在哪里发生，
ODBC 与 libpq 一样：名字当年是带引号、大小写混着建出来的那套库，靠 `keep` 或逐名
解析点名，默认折叠不会替它对上。`validate()` 按同一个策略去问目录，落空的
消息点名目录里那一侧的拼法：

    table not found: USER_ACCOUNTS (only case differs from 'user_accounts')

折叠发出的是折出来的拼法，不是声明的那一个：库里同时存在 `"Users"` 与 `"users"`
两张表时，声明 `Users` 在 `lower` 之下读到的是小写那张，`validate()` 也说命中。
要点名实际写下的那张，得用下面的逐名解析。

一个大小写策略不能适配所有映射——MariaDB 可能将表名存成小写、列名保留大写。
这时可在注册映射后显式逐名解析：

```cpp
db.resolve_identifiers("app_db", "public");  // PostgreSQL
// db.resolve_identifiers("app_db", "");    // MySQL / MariaDB
db.validate();
```

精确名称优先，否则只接受唯一的 ASCII 大小写候选；缺失或歧义抛
`mapping_error`，所有映射一起安装结果，任意一个失败则全部保留原状。已解析映射
使用实际名称，不再受 `identifier_case` 影响。PostgreSQL 要求当前数据库和显式
schema；MySQL/MariaDB 要求 database 和空 schema。首版只支持普通表，后端不能保证
精确元数据读取时明确报不支持——libpq 这一家还没接目录读取，这条腿上 `schema()`
与 `validate()` 都是直接抛出。`clear_identifier_resolution()` 恢复原来的大小写
策略，重连、断开也会清除结果。手写 SQL 不改写；动态表构建器不参与解析，继续
使用连接的大小写策略。

### 批量写入

```cpp
std::vector<User> users = /* ... */;
std::size_t n = db.insert(users);   // NULL 与按 paramset_size 分批全自动；
                                    // 连接 autocommit 时包进事务
std::size_t u = db.update(users);   // 默认按全部主键列匹配
std::size_t r = db.remove(users);   // 同上，可传 where_fields 覆写

// 单行是同一组重载去掉 vector；where_fields 是列名，批量形式同样适用
std::size_t one = db.remove(users.front(), { "name" });  // 只按 name 匹配

// 无人映射的表走动态构建器（动态构建器没有 insert：没有映射，就没有一组列）。
// 链式 where() 累加并 AND，与 query<T>::where 一致。
std::size_t m = db.update("users")
                  .set("age", 31)
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .where("name = ?", uniorm::params{ std::string("bob") })
                  .execute();
std::size_t d = db.remove("users")
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .execute();
```

两个尺寸可在 `orm` 上设，传 0 会被夹回默认值：`paramset_size`（默认 1000）
每次绑定执行多少行，`row_array_size`（默认 100）每条读取路径每次 fetch 物化
多少行。

### 事务与连接池

```cpp
{
    auto txn = db.begin();
    db.execute_update("INSERT INTO logs (msg) VALUES (?)",
                      uniorm::params{"x"});
    txn.commit();  // 不 commit 则析构时回滚
}

db.auto_commit(false);            // 就是连接的 autocommit 属性：此后每一条
db.execute_update("DELETE FROM logs", uniorm::params{});
                                  // 写都挂起，直到调用方
db.commit();                      // commit()，批量写也不例外

uniorm::pool_options opts;
opts.target = "DSN=mydb;UID=user;PWD=secret";
                                  // 或四件套：
                                  // opts.target = uniorm::connection_spec{
                                  //     "odbc", "mydb", "user", "secret" };
opts.size = 8;                    // 池最多持有多少条连接
opts.acquire_timeout = std::chrono::seconds{ 5 };
opts.heartbeat_interval = std::chrono::seconds{ 30 };  // 0 = 不起维护线程
opts.max_idle_time = std::chrono::minutes{ 10 };
opts.heartbeat_sql = "SELECT 1";
uniorm::connection_pool pool(opts);  // 连接按需懒创建

{
    uniorm::orm leased(pool);       // 构造即借出、析构归还；无空闲时抛
    leased.execute_update("...");   // pool_timeout
}

auto slot = pool.acquire();         // 同样的租用，只是不套 orm 层：一个只可移动
                                    // 句柄，析构即归还
std::size_t held = pool.capacity(); // 另有 idle_count() 与
                                    // heartbeats_executed()
```

所有池共用**一条**后台调度线程，`heartbeat_interval` 非零的池会挂上去：它周期性
对空闲连接执行心跳，心跳失败即丢弃，空闲超过 `max_idle_time` 的彻底释放——
`acquire()` 交出连接前也会执行这条规则。归还的连接先被重置，重置失败就直接退休。
池必须活得比所有还没归还的连接久。

`orm("DSN=…")` 不需要你自备池：它从一个以 DSN + UID 为键的进程级注册表里借，
每个池按默认 `pool_options` 建立。想改这些默认值，就对该连接串调用
`connection_pool_registry::configure()`。

### 异步

这里没有第二条数据库通路：一个 task 的函数体**就是**它包着的这次同步调用——
`orm::execute_update`、那个取行循环、整段批量扫描——跑在一条调用方不等着的
线程上。ODBC 连接器没有原生异步入口，所以这一层是仿真；哪一家 backend 真有
原生异步，就把它换掉，调用方一行不改。

```cpp
#include <uniorm/async.hpp>

uniorm::async::task<void> rename(uniorm::async::orm& db) {
    auto tx = co_await db.begin();
    co_await db.execute_update("UPDATE users SET name = ? WHERE id = ?",
                               uniorm::params{"Ada", 7});
    co_await tx.commit();   // 漏掉这一句会回滚，而那一次是阻塞的
}

int main() {
    uniorm::async::orm db;
    db.map<User>("users")                  // 配置面是按名字转发出去的
        .primary_key("id", &User::id)
        .column("name", &User::name);
    db.connect("DSN=mydb;UID=user;PWD=secret").block();

    rename(db).block();                    // 停住这条线程直到它跑完
    db.execute_update("DELETE FROM visits", uniorm::params{})
      .start([](std::exception_ptr bad) {  // 交出去，自己接着走
        if (bad) { /* 这次写丢了，handler 是唯一痕迹 */ }
      });
    auto names = db.query<User>().all().block();
}                                          // ~orm 等它交出去的一切落地
```

这个形状承诺五件事：

- **worker 条数就是并发上限。** 第一个 task 之前调用
  `default_executor_size(n)` 定下它；此后只会增。要并行就多开几把会话，而不是
  在一把会话上多排任务。
- **同一把会话上排队不等于并行。** 一把会话同时只有一个在飞，其余按被 drive
  的先后跑——`co_await`、`block()`、`start()` 共用这一条 FIFO。排队的请求不占
  线程；`pending()` 数它们。超过 `submit_limit()`（默认 1024）时 drive 动作当场
  抛 `submit_overflow`，并把 task 留在未驱动状态，过一会儿再试是合法写法。
- **失败只有一个出口。** `start()` 收一个 `on_failure`，它只为失败而响；
  `block()` 与 `co_await` 则把同一个 `exception_ptr` 重抛出来。
- **续体默认换线程。** 链条的下一环落在跑完那次读的 worker 上。
  `run_on(executor&)` 说阻塞的活去哪儿，`resume_on(executor&)` 说回到哪儿；
  一次性脚本两个都不必写。
- **四处阻塞点是承认下来的，不是漏掉的：** 留在同步侧的目录读（`validate()`、
  `schema()`、`resolve_identifiers()`，经 `sync()` 调用）、
  `~async::transaction` 兜底那次回滚，以及 `~orm()` 对本会话交出工作的等待
  （同一个等待的公开出口是 `wait_all_operations()`）。

调一句、听一次，用不着上面那一串，所以 `insert` / `update` / `remove` /
`query<T>` 各还有一层收回调的重载：

```cpp
db.insert(users, [](std::size_t added, std::exception_ptr bad) {
    if (bad) { /* 这时 added 是 0，不是"部分成功" */ return; }
    std::printf("%zu rows in\n", added);
});

db.query<User>("SELECT id, name FROM users WHERE id > ?",
               uniorm::params{std::int64_t{9000}},
               [](std::vector<User> rows, std::exception_ptr bad) {
    if (bad) { return; }
    use(rows);
});
```

**先看第二个参数**：失败时第一个是 `R{}`。回调跑在"跑了这次操作的那条线程"上，
而且是在这一格还没交回去的时候——回调慢，同一把会话的下一条请求就跟着等；换来的
是 `~orm()` 一定会等到自己欠的每个回调，所以按引用捕获局部变量是合法的。这一层
只覆盖四个 CRUD 名字：builder 的三个终端、`connect`、`begin` 仍是 task 形。

### 代码生成（uniorm-gen）

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
uniorm-gen --input-ddl=schema.sql --out=build/gen [--name=app]
```

`--dsn` / `--connection-string` / `--input-ddl` 三者只能选一个，
`--out` 则必须给。`--catalog` 与 `--schema` 收窄去哪找表，按名字而不是通配模式：
值里的 `%` 不匹配任何东西（留空即由连接说了算）。`--name` 改输出单元名，
`--tables` 取逗号分隔的表名列表。

`--input-ddl` 读的是一份建表脚本而不是活服务器，那一趟不开连接。它认 MySQL 与
PostgreSQL 两家的 `CREATE TABLE`（列与表级键）、`CREATE INDEX` 以及
`ALTER TABLE` 里加东西的那些形式；认得出但跟映射无关的动作（`DROP`、`MODIFY`、
存储子句）计数后跳过，末尾并成一条警告。`USE app;` 给缺省单元名。

输出 `build/gen/<name>_schema.hpp`：每表一个 struct（可空列自动
`std::optional`，DECIMAL/NUMERIC 生成为无损的 `std::string`），PK/FK/索引
信息以注释输出，并生成 `register_<name>_schema(uniorm::orm&)` 注册函数；
`<name>` 默认取数据库名，其中实在没有可用字符时取 `db`。
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

覆写仅限注册表可绑定的类型。配置里的键不区分大小写；指向不存在的表或列的那一段
会在生成前报错而不是被静默忽略，两张表折成同一个类名同样当场报错。完整规格见
设计文档 §6。

## 测试

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**：纯内存测试，基于 fake backend 与 fake 目录运行，且不链接任何
  backend 库，公共 API 一旦泄漏驱动类型即编译失败。它还收纳生成器的配置与
  输出用例
- **odbc_unit_tests**：私有 ODBC 句柄层、它的错误转换，以及目录行按名字再筛
  那条判据（仅驱动管理器，无需 DSN）
- **oracle_oci_unit_tests**：Oracle backend 自己那一层——占位符改写、它的类型表，
  以及连接串怎么切分（仅客户端库，无需服务器）
- **libpq_unit_tests**：libpq backend 自己那一层——占位符改写、它的类型表，以及
  连接串怎么切分（仅客户端库，无需服务器）
- **integration_tests**：需要可达的 ODBC DSN，读取环境变量 `UNIORM_IT_DSN`、
  `UNIORM_IT_USER`、`UNIORM_IT_PWD`（三者均须设置）；任一未设置或连不上时以
  ctest SKIP 处理
- **integration_tests_oracle**：同一份套件走 Oracle plugin，
  `UNIORM_IT_BACKEND=oracle`，target 按 `host[:port]/service` 读而不是按 DSN；
  读目录的那几个阶段自己报 skip
- **integration_tests_postgresql**：同一份套件走 libpq plugin，
  `UNIORM_IT_BACKEND=postgresql`，target 按 `host[:port]/dbname` 读而不是按
  DSN；读目录的那几个阶段自己报 skip。丢掉已准备语句的那一段只在按名字持有它的
  backend 上跑，其余腿报 skip
- **perf_tests**（ctest 标签 `perf`）：ODBC 这条腿，批量写与三条查询物化
  路径，旁边是调用形式逐项配对的纯 ODBC 基线，用作抽象开销参照；行数由
  `UNIORM_PERF_ROWS` 指定（默认 10000）；可用 `ctest -LE perf` 跳过
- **perf_tests_oracle**：同一份 uniorm 用例走 Oracle plugin，旁边是手工
  调用 OCI 的基线；plugin 不向 client 要的那几种形状（数组 DML、按行
  prefetch）单独成行，前提是这一家 client 收数组绑定——不收时那三行自己
  问一次就退场；target 按 `host[:port]/service` 读
- **perf_tests_libpq**：同一份 uniorm 用例走 libpq plugin，旁边是手工调用
  libpq 的基线——那里的批量就是一行一条 prepared statement；一条语句带
  一千行 VALUES 列表那种写法单独成行；target 按 `host[:port]/dbname` 读
- **gen_e2e_tests**：生成物与检入的 golden 头文件比对（注释不比），golden 本身
  经编译、注册并 `validate(strict)`；需可达 DSN
- **install_smoke**：`cmake --install` 装进构建树下的临时 prefix，再用一个外部
  工程（`tests/install/`）经 `find_package(uniorm CONFIG)` 配置、编译并运行，且
  它自己不设 C++ 标准

三个读 DSN 的目标只在 `UNIORM_BACKEND_ODBC=ON` 时存在，`gen_e2e_tests`
还额外要求 `UNIORM_BUILD_TOOLS=ON`。

## 目录结构

```
include/uniorm/       公共头文件（消费者唯一需要的 include 根）
  backend/            驱动中立的 backend 接口、注册表、错误体系
  detail/             pfr-lite、投影绑定、chrono 工具
  mapping/            实体映射注册表
  builder/            谓词表达式与流式查询/更新/删除构建器
  schema.hpp          每个 backend 都要应答的自省契约
  async.hpp           task<T>、executor、async::orm、async::transaction
                        （不进总头文件，协程因此是按需 include 的）
src/                  实现（构建为 libuniorm.so）
  async/              默认 worker 池、会话那条 FIFO
  backend/            scheme 解析、注册表与 plugin loader
  statement_cache.hpp 预编译语句 LRU 缓存
  orm_mapping.hpp     实体写入按什么匹配、赋哪些列
backends/odbc/        ODBC backend（全仓库唯一出现 <sql.h> 之处）
backends/oracle_oci/  Oracle OCI backend（唯一出现 <oci.h>，默认不构建）
backends/libpq/       libpq backend（唯一出现 <libpq-fe.h>，默认不构建）
cmake/                装出去的 find_package 所读的包配置模板
tools/uniorm-gen      代码生成 CLI
tests/unit            单元测试
tests/integration     数据库集成测试（含 uniorm-gen 的 golden 头文件）
tests/install         外部消费者工程
tests/perf            性能基准测试
docs/design.md        设计文档（权威 API 参考）
```

## 状态

v1 已完成，且每一条都对活库验过：三条访问路径（裸 SQL / 聚合投影 / 实体映射）、
批量写入、事务、连接池、方言层、`uniorm::decimal_t`，以及 `uniorm-gen` 端到端。
同一套测试在 MariaDB 上、经 Connector/ODBC 走服务端预处理在真 MySQL 上、经
psqlODBC 在 PostgreSQL 上全绿。

v2 进行中，v1 之上另外落地的有：驱动中立的 backend 抽象——每家 backend 如今是
一份独立 plugin，核心在运行时按 scheme 载它——自省成为公开契约（`schema()`）、
标识符拼法成为部署策略、`connection_spec`、同一个契约之上的异步层，以及按同一
个契约长出来的另外两家 backend（Oracle OCI、libpq）。

CI 是五支作业定义、七个实例：`core`、`core-windows`、`oracle-oci` 与 `libpq`
各自一支，`driver` 带三条腿，每条拿自家的连接器连自家会用的服务端。每个实例还
各自把每个安装头单独编成一个翻译单元，公共头若要靠邻居先 include 才过得去，红
的是这里而不是消费者的工程；任何一条测试被跳过都会让一支腿红，所以一支腿绿就
意味着确实连上了服务端。libpq 之外的那六个实例在 runner 上已全绿；libpq 这一支
是在手边复放过一遍活容器，runner 自己那一半——apt 的 libpq、镜像拉取、健康门——
还没验过。

未结：OCI 那家还没接上的原生数组绑定，以及两家原生 backend 都欠着的目录读取，
见设计文档 §5.4 与 §9。

## 许可证

MIT，见 [LICENSE](LICENSE)。
