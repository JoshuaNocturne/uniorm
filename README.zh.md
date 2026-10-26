# uniorm

[English](README.md)

现代 C++20 数据库访问层。核心是驱动中立的：连接串里的 scheme 选择 backend。
默认构建的那一家是 ODBC——也正因如此，uniorm 能访问任意提供 ODBC 驱动的数据库，
而不依赖特定厂商的 C 客户端；Oracle OCI backend 由构建开关显式打开。
实体映射、成员指针查询构建、
事务与连接池都建在这层核心之上。

详细设计见 [docs/design.md](docs/design.md)。

## 特性

- **同步 API + 异常错误体系**：所有失败以 `uniorm_error` 派生树抛出
  （`column_not_found`/`type_mismatch`/`mapping_error`/`pool_timeout`）；驱动侧
  失败以 `backend::backend_error` 到达并携带 SQLSTATE 诊断，backend 答不了的
  读取以 `backend::capability_not_supported` 到达
- **内部统一 UTF-8**：字符串一律以窄字符（`SQL_C_CHAR`）绑定；编码转换发生在
  驱动侧，不在本库
- **预编译 + 绑定变量**：用户值一律经 `SQLBindParameter`，杜绝拼接注入
- **透明的语句缓存**：按 SQL 文本的 LRU 缓存，重复执行免 prepare
  （观测：`statement_cache_hits()/misses()/statement_cache_size()`，
  `clear_statement_cache()` 清空）
- **三种使用层次**：
  - 裸 SQL：`execute` / `execute_update` + `params`
  - 聚合投影：`db.query<Row>(sql)` 零注册按列序映射到 struct
  - 实体映射：注册表 + 类型安全的成员指针谓词构建器 `db.query<T>()`
- **实体直接绑定**：`query<T>::all()/one()` 将结果列直接绑到实体字段
  （`SQLBindCol`），跳过行物化
- **自定义类型映射**：特化 `uniorm::converter<T>` 即命名域类型所绑定的 SQL
  表示，该表示带着域类型走通实体字段、参数与投影；`validate()` 的 strict 模式
  （默认）会拿它与活库的列类型比对
- **精确定点小数值**：`uniorm::decimal_t` 存尾数 + scale，按值比较、逐值零分配；
  DECIMAL/NUMERIC 列默认生成为无损的 `std::string` 成员，按列 `cpp_type` 覆写即
  换成 `decimal_t`，而它绑定的正是这个字符串表示
- **批量写入**：`db.insert(rows)` 与批量 `db.update(rows)` /
  `db.remove(rows)`：一条占位符语句经数组参数绑定
  （`SQL_ATTR_PARAMSET_SIZE`）展开，按 `paramset_size` 分批；连接处于
  autocommit 时整批扫描包进事务，手动模式下这些批次本就在调用方的事务里
- **RAII 事务**：析构自动回滚
- **可选的异步表面**（`async.hpp`）：同一个同步契约套上
  `uniorm::async::task<T>`：一把会话同时只跑一个操作，三种 drive 动作
  （`co_await`、`block()`、`start()`），跑与回两个方向都能交给调用方自己的
  executor。四个 CRUD 名字还各有一层收回调的重载，给"调一句、听一次"的用法
- **连接池**：懒创建、借还超时；所有池共用一条全局维护线程执行心跳保活与
  空闲超时回收
- **活库 schema 自省**：`orm::schema()` 与 `connection::schema()` 交出一个
  `uniorm::schema_meta`：库名、表清单，以及每张表的列、主键、外键与索引。
  `shape()` 把一张表约成 `column_shape` 列表——`validate()` 校验映射时读的正是
  它——于是生成器与校验器走同一份契约
- **方言自适应**：标识符引号与分页语法按 `SQL_DBMS_NAME` 推断
  （MySQL/MariaDB 用反引号 + LIMIT/OFFSET，其余 ANSI）；标识符拼法另有一把部署级
  开关，`keep`（默认，原样）/ `lower` / `upper`，作用在标识符进 SQL 的那一处，
  `validate()` 问目录时也按它折
- **代码生成**：`uniorm-gen` 连活库，经 backend 自省读取 schema，生成实体
  struct + 注册函数（TOML 覆写类型/类名/跳过表）
- **可插拔 backend**：核心 API 构建在驱动中立的 backend 接口之上，连接串
  scheme 选择后端（`odbc://...`、`oracle://...`；裸 ODBC 连接串保持向后兼容）。
  ODBC 默认就编，够到任何有驱动的服务；Oracle OCI backend 要显式打开。另一种
  写法是 `connection_spec`：scheme、target、
  user、password 四件，交出去时不拼词，怎么拼由那一家 backend 说了算。
  目录读取也是这份接口的一部分（`schema()`），没有目录
  的 backend 于是答 `capability_not_supported`，而不是在某次读取内部失败；这里的每
  个名字都按名字问、不按 pattern 问，一次读取因此只带回服务端在该名下报出的行。能力
  清单里只有核心真会分岔的两条：`columnar_batch` 选批量写的那条通道，
  `array_rowcount_totals` 决定数受累行数的扫描怎么分批

## 要求

- C++20 编译器——构建只要求 `cxx_std_20`，不编码任何版本下限；CI 用
  Ubuntu 24.04 的默认 GCC 编译
- CMake ≥ 3.20
- unixODBC（`find_package(ODBC)`）及目标数据库的 ODBC 驱动——仅在
  `UNIORM_BACKEND_ODBC=ON` 时需要
- Oracle Call Interface（Instant Client，或完整客户端的 SDK）——仅在
  `UNIORM_BACKEND_ORACLE_OCI=ON` 时需要；用 `OCI_ROOT` 或 `ORACLE_HOME` 指到那棵树

## 构建

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

常用开关：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | Debug / Release / RelWithDebInfo / MinSizeRel |
| `UNIORM_BUILD_TESTS` | `ON` | 构建单元/集成/性能测试 |
| `UNIORM_BUILD_TOOLS` | `ON` | 构建工具（`uniorm-gen`） |
| `UNIORM_BACKEND_ODBC` | `ON` | 将 ODBC backend 编入 `libuniorm`；关闭后只剩一个没有任何 backend 的核心库，此时 `UNIORM_BUILD_TOOLS=ON` 是配置错误 |
| `UNIORM_BACKEND_ORACLE_OCI` | `OFF` | 构建 Oracle OCI backend（目标 `uniorm_oracle_oci`，装出去仍叫 `libuniorm_oracle`——核心按 `libuniorm_<scheme>.so` 加载，scheme 是 `"oracle"`）；配置它的那台机器上要有 Oracle Call Interface |

产物为动态库 `libuniorm.so`（头文件在 `include/uniorm/`）：目标一律是
`SHARED`，`BUILD_SHARED_LIBS` 改变不了它。`VERSION` 为 0.3.0，
`SOVERSION` 取 major.minor（0.3）。逐名解析改变了公共布局和元数据虚表，
消费者与第三方 backend 必须重新编译，不可混用 0.2 的头文件或二进制。

### 安装

```sh
cmake --install build --prefix /path/to/prefix
```

带版本号的动态库（含 `SOVERSION` 两级符号链接；Windows 下为 DLL 与导入库）进
`<libdir>/`，全部 public 头文件进 `<prefix>/include/uniorm/`，包配置进
`<libdir>/cmake/uniorm/`，`uniorm-gen` CLI 进 `<bindir>/`。消费者不需要本仓库的
任何其他文件：

```cmake
find_package(uniorm REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE uniorm::uniorm)
```

C++20 的要求随目标一起导出，消费者无需自己再设 `CMAKE_CXX_STANDARD`。

源码集成方式不变：`add_subdirectory` 或 FetchContent 同样拿到 `uniorm::uniorm`
目标，且不安装任何东西。安装规则只在 uniorm 是顶层工程时生效。CLI 只在
`UNIORM_BUILD_TOOLS=ON`（该选项要求 ODBC backend 开启）时安装，且是"可执行程序"
而非"可链接目标"：`find_package` 仍然只导出 `uniorm::uniorm`。

## 快速上手

### 连接与裸 SQL

```cpp
#include <uniorm/uniorm.hpp>

uniorm::orm db("DSN=mydb;UID=user;PWD=secret");
// 等价的显式写法："odbc://DSN=mydb;UID=user;PWD=secret"。
// :// 之前的 scheme 选择 backend；无 scheme 的串按 ODBC 连接串处理
// （这需要把 ODBC backend 编进来——关掉它的构建会抛 unknown_scheme）。
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
`orm::connect(...)` 收同一个形状。它并不声称够用——驱动关键字、端口、自己的
库名、TLS 策略、登录超时这些都仍归串管，而串是每个 backend 都要实现的原语。
池上因此有一条后果：同一个库，一份用 spec 建、一份用串建，是两个池——判定它们
指的是同一个库要的是 driver 的词汇，core 没有。

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
#include <uniorm/uniorm.hpp>

struct User {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

uniorm::orm db("DSN=mydb;UID=user;PWD=secret");
db.map<User>("users")
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age);

db.validate();  // 与活库 schema 对账（可选）；默认 strict：表或列不在、
                // 成员绑不了该列的类型、可空列背后是非 optional 成员，都抛
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
里写明 schema。这个引用属于所借的那条连接，`disconnect()` 或重新借出都会让它
失效，别缓存它。

### 标识符拼法

映射可以保留声明名，也可以在生成 SQL 时统一转换大小写。不加引号的 DDL 在
PostgreSQL 上通常存成小写；MySQL/MariaDB 的表名拼写还取决于服务器配置。
目录保留的拼写和数据库比较名称时是否区分大小写，是两件不同的事。

```cpp
db.identifier_case(uniorm::dialect::identifier_case::upper);  // 默认不声明
```

不声明时跟着服务器自己那家方言走：Oracle 把不加引号的名字折成大写，那里什么都不
用声明；其余家原样发出声明的名字。把 `keep` 声明出来是一个决定，不是没有决定。
`lower` 与 `upper` 只折 ASCII 字母，别的字母表里的名字不会被折到另一个名字上。
连接之后声明一次即可，`orm` 每次租到连接都会重贴上去，与 `auto_commit` 同一个
解法；撤掉声明就把答案交还给服务器。`validate()` 按同一个策略去问目录，落空的
消息点名目录里那一侧的拼法：

    table not found: USER_ACCOUNTS (only case differs from 'user_accounts')

表名只在目录里以另一种大小写存在时，就报在表名那一层：有的服务器比名字不分大小写，表
会同它的列一起答回来，但那些列是顺带约来的，错的是表名。

一个大小写策略不能适配所有映射。例如 MariaDB 可能将表名存成小写、列名保留大写；
SQL 能执行，但精确拼写校验仍会拒绝。这时可在注册映射后显式逐名解析：

```cpp
db.resolve_identifiers("app_db", "public");  // PostgreSQL
// db.resolve_identifiers("app_db", "");    // MySQL / MariaDB
db.validate();
```

精确名称优先，否则只接受唯一的 ASCII 大小写候选；缺失或歧义抛 `mapping_error`。
所有映射一起安装结果，任意一个失败则全部保留原状。声明名不改写，显式 update/remove
WHERE 字段仍写声明名；再次解析也始终从声明名出发。

已解析映射使用实际名称，不再受 `identifier_case` 影响。PostgreSQL 要求当前数据库
和显式 schema；MySQL/MariaDB 要求 database 和空 schema。生成 SQL 分别带上 schema
或 database 限定。首版只支持普通表，不猜 search_path、不跨 schema 寻找，后端不能
保证精确元数据读取时明确报不支持。

`validate()` 不解析、不修改映射；对已解析映射精确重读原来选中的对象，不会因对象
消失而改选另一个。`clear_identifier_resolution()` 恢复原来的大小写策略；重连、
断开也会清除结果，重连失败同样清除。扩展已解析映射前需要先清除；新注册的实体先保持
未解析，下一次调用再统一解析。这些操作不能与查询并发进行。保留 query 时，必须让
它依赖的 gateway 和 ORM 继续存活。已解析映射的 `populate()` 使用实际列标签。

手写 SQL 不改写；动态表构建器不参与解析，继续使用连接的大小写策略。

### 批量写入

```cpp
std::vector<User> users = /* ... */;
std::size_t n = db.insert(users);   // NULL 与按 paramset_size 分批全自动；
                                    // 连接 autocommit 时包进事务
std::size_t u = db.update(users);   // 默认按全部主键列匹配
std::size_t r = db.remove(users);   // 同上，可传 where_fields 覆写

// 单行是同一组重载去掉 vector；where_fields 是列名，批量形式同样适用
std::size_t one = db.remove(users.front(), { "name" });  // 只按 name 匹配

// 无人映射的表走动态构建器。动态构建器没有 insert：没有映射，就没有一组列
std::size_t m = db.update("users")
                  .set("age", 31)
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
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
`acquire()` 交出连接前也会执行这条规则。归还的连接先被重置（回滚未提交的工作、
恢复 autocommit），重置失败就直接退休，不把脏句柄交出去。池必须活得比所有还没
归还的连接久。

`orm("DSN=…")` 不需要你自备池：它从一个以 DSN + UID 为键的进程级注册表里借，
每个池按默认 `pool_options` 建立。想改这些默认值，就对该连接串调用
`connection_pool_registry::configure()`。

### 异步表面

这里没有第二条数据库通路：一个 task 的函数体**就是**它包着的这次同步调用——
`orm::execute_update`、那个取行循环、整段批量扫描——跑在一条调用方不等着的线程
上。驱动自己的异步入口一个都没用到，因为连接器根本没有。

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
  `default_executor_size(n)` 定下它；此后只会增，因为一条已经进了驱动调用的
  worker 拔不出来。要并行就多开几把会话，而不是在一把会话上多排任务。
- **交出去的失败只有一个出口。** `start()` 收一个 `on_failure`，它只为
  失败而响：跑成的那次不报，抛了的那次也只报在这里。`block()` 与 `co_await`
  则把同一个 `exception_ptr` 重抛出来。handler 自己抛出停在库这一侧：它是失败
  与收这条失败的那条线程之间的最后一段，而跑它的两处都不许展开。
- **同一把会话上排队不等于并行。** 一把会话同时只有一个在飞，其余按被 drive
  的先后跑——`co_await`、`block()`、`start()` 共用这一条 FIFO。排队的请求不占
  线程；`pending()` 数它们，超过 `submit_limit()`（默认 1024）时 drive 动作当场
  抛 `submit_overflow`，并把 task 留在未驱动状态，过一会儿再试是合法写法——
  只有 `co_await` 没有可抛的调用点：那一次拒绝折成这一次 await 自己的失败，
  从函数体的 `catch` 里出来，和驱动报的错落在同一处。
  `wait_all_operations()` 是 `~orm()` 里那道屏障的显式出口：会话还开着的时候
  等一次"交出去的都落地"，长驻会话关闭又不想此刻断连接才用得上；
  它只等已受理的请求。
- **续体默认换线程。** 链条的下一环落在跑完那次读的 worker 上。
  `run_on(executor&)` 说阻塞的活去哪儿，`resume_on(executor&)` 说回到哪儿；
  一次性脚本两个都不必写。会话的入口与同步对象同款三条——连接串、四件套、从
  pool 借还——所以 `uniorm::orm` 不必出现在代码里。`map` 与行、批量、标识符那
  几个旋钮是按名字转发给内层那一个对象的，它们属于"没有在飞操作"的那些时刻——
  改的正是这条队列要保护的那份状态。
- **四处阻塞点是承认下来的，不是漏掉的：** 留在同步侧的目录读（`validate()`、
  `schema()`、`resolve_identifiers()`，经 `sync()` 调用）、
  `~async::transaction` 兜底那次回滚、`~orm()` 对本会话交出工作的等待，以及
  同一个等待的公开出口 `wait_all_operations()`。同一次析构还会在连接仍然活着时
  把没闭合的事务句柄逐个回滚：句柄允许活过会话，之后再让它 `commit()`，它报的是
  会话没了，而不是去踩一条已经关掉的连接。

调一句、听一次，用不着上面那一串，所以 `insert` / `update` / `remove` /
`query<T>` 各还有一层收回调的重载。它是同一把会话的第二张脸，不是第二条通路：
交给队列的那次操作，与协程那面 `co_await` 的是同一个。

```cpp
// db 还是上面那个会话：已经连上，User 的映射也在上面。
// 它不知道一次请求是从哪张脸进来的。
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
是 `~orm()` 一定会等到自己欠的每个回调，所以按引用捕获局部变量是合法的。空的
`on_done` 在调用点就被拒：它什么都答不了，却要让析构等一次永远不会响的回调。它
自己抛出就停在抛出的地方——那一次答案已经给过，会话照样接下一条。队列满时照样
抛在调用点（与 `block()`、`start()` 同一条规矩），被拒的那次请求之后也不会有
回调——根本没问出去。`resume_on` 对它没有话语权，它没有续体可交；`run_on` 仍有：
会话挂着一台只有测试会泵的手泵，析构就等不到那一格交回来。这一面只覆盖四个
CRUD 名字：
builder 的三个终端、`connect`、`begin` 仍是 task 形。

### 代码生成（uniorm-gen）

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
uniorm-gen --input-ddl=schema.sql --out=build/gen [--name=app]
```

`--dsn` 与 `--connection-string` 是同一个连接的两种写法——前者把
`{"odbc", <dsn>, <user>, <password>}` 交给消费者自己也会用的那个
`uniorm::orm` 构造器，怎么拼、要不要转义归 ODBC backend——`--dsn` /
`--connection-string` / `--input-ddl` 三者只能选一个，
`--out` 则必须给。`--catalog` 与 `--schema` 收窄去哪
找表，按名字而不是通配模式：值里的 `%` 不匹配任何东西，运行会先这么说（留空即由
连接说了算）。`--name` 改输出单元名，`--tables` 取逗号分隔的表名列表。

`--input-ddl` 读的是一份建表脚本而不是活服务器，那一趟不开连接。它认 MySQL 与
PostgreSQL 两家的 `CREATE TABLE`（列与表级键）、
`CREATE INDEX` 以及 `ALTER TABLE` 里加东西的那些形式；认得出但跟映射无关的动作
（`DROP`、`MODIFY`、存储子句）计数后跳过，末尾并成一条警告，不至于让整次运行
失败。脚本说不出一个未限定的表住在哪个 catalog/schema，所以这样的表对你传的
任何一个 `--catalog` / `--schema` 都答。`USE app;` 给缺省单元名。

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

覆写仅限注册表可绑定的类型。配置里的键不区分大小写，指向不存在的表或列的那一段
会在生成前报错而不是被静默忽略（`skip` 写错最坏）；两张表折成同一个类名同样当场报错
并点名它们，同一个 `struct` 声明两次的头不是交付物。名字不只差大小写的一对，
一个 `class` 覆写就能分开；只差大小写的一对共享同一个配置键，指不到其中单独
一张，于是运行改让你用 `--tables` 只取其中一张。完整规格见设计文档 §6。

## 测试

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**：纯内存测试，无外部依赖；基于 fake backend 运行（`validate()`
  那组则是 fake 目录），且不链接 ODBC，公共 API 一旦泄漏驱动类型即编译失败。
  `UNIORM_BUILD_TOOLS=ON` 时它还收纳生成器的配置与输出用例——那部分逻辑已不点名
  任何驱动，于是住进那个不链驱动的测试目标
- **odbc_unit_tests**：私有 ODBC 句柄层、它的错误转换，以及目录行按名字再筛的那条
  判据（仅驱动管理器，无需 DSN）；只在 `UNIORM_BACKEND_ODBC=ON` 时构建
- **oracle_oci_unit_tests**：Oracle backend 自己那一层——占位符改写、它的类型表，以及
  连接串怎么切分（仅客户端库，无需服务器）；只在 `UNIORM_BACKEND_ORACLE_OCI=ON` 时构建
- **integration_tests**：需要可达的 ODBC DSN，读取环境变量
  `UNIORM_IT_DSN`、`UNIORM_IT_USER`、`UNIORM_IT_PWD`（三者均须设置），
  凭据以 `UID`/`PWD` 拼入连接串；任一未设置或连不上时以 ctest SKIP 处理
- **integration_tests_oracle**：同一份套件走 Oracle plugin，`UNIORM_IT_BACKEND=oracle`，
  target 按 `host[:port]/service` 读而不是按 DSN；只在 `UNIORM_BACKEND_ORACLE_OCI=ON`
  时构建，连不上时 SKIP。读目录的那几个阶段自己报 skip——这个 backend 还没答这一项
- **perf_tests**（ctest 标签 `perf`）：批量插入吞吐与三条查询物化路径
  （实体直绑 / 聚合投影 / 动态行）的对比，并附调用形式与 uniorm
  一一对应的纯 ODBC 基线作为抽象开销参照；行数由 `UNIORM_PERF_ROWS` 指定，
  未设置、为 0 或超过一百万时一律回到 10000；可用 `ctest -LE perf` 跳过
- **gen_e2e_tests**：`uniorm-gen` 端到端——生成物的代码与检入的 golden 头文件
  比对（注释不比），golden 本身经编译、注册并 `validate(strict)`；需可达 DSN
- **install_smoke**：安装面检查——`cmake --install` 装进构建树下的临时
  prefix，再用一个外部工程（`tests/install/`）经 `find_package(uniorm CONFIG)`
  配置、编译并运行，且它自己不设 C++ 标准。它同时还检查包文件是否齐、装出去的
  头文件集合是否等于 `include/uniorm/`（安装规则一旦开始筛头文件就会在这里暴露），
  以及向 `find_package` 要 `99.0.0` 会不会被 `SameMinorVersion` 门拒掉；
  `UNIORM_BUILD_TOOLS=ON` 时还要跑一次装出来的 `uniorm-gen`，prefix 相对 RPATH
  坏了就在这里现形。任何顶层、非交叉编译的构建都会跑它，与 ODBC backend 开不开
  无关；只有最后一项取决于工具开关

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
  async.hpp           task 形状的那一面，只有声明与模板：task<T>、
                        executor、async::orm、async::transaction
                        （不进总头文件，协程因此是按需 include 的）
src/                  实现（构建为 libuniorm.so）；私有头贴着对应 .cpp 存放，
                        以相对名引用，不在任何对外 include 路径上
  async/              默认 worker 池、会话那条 FIFO，以及除模板之外的全部
                        异步函数体
  backend/            scheme 解析与 backend 注册表
  statement_cache.hpp 预编译语句 LRU 缓存
  orm_mapping.hpp     实体写入按什么匹配、赋哪些列——只从映射推导，不碰连接
backends/odbc/        ODBC backend（适配器、句柄 RAII 封装、错误，以及应答
                        schema() 的目录读取）
                        全仓库唯一出现 <sql.h> 之处
backends/oracle_oci/  Oracle OCI backend（会话句柄、`?` 到 `:n` 的改写、它自己的
                        类型表）
                        全仓库唯一出现 <oci.h> 之处，默认不构建
cmake/                装出去的 find_package 所读的包配置模板
tools/uniorm-gen      代码生成 CLI（schema 提取 + TOML 配置 + 生成器）
tests/unit            单元测试
tests/integration     数据库集成测试（含 uniorm-gen 的 golden 头文件）
tests/install         外部消费者工程（install_smoke 用它验证安装面）
tests/perf            性能基准测试
docs/design.md        设计文档（权威 API 参考）
```

## 状态

### 已交付

v1 已完成：三条访问路径（裸 SQL / 聚合投影 / 实体映射）、批量写入、事务、连接池、
方言层、`uniorm::decimal_t`，以及 `uniorm-gen` 端到端，每一条都对活库验过。同一套
测试在 MariaDB 上、经 Connector/ODBC 走服务端预处理在真 MySQL 上、经 psqlODBC 在
PostgreSQL 上全绿。夹具表改成三家都认的写法，于是一份 golden 便供三家。

v2 进行中，v1 之上另外落地五块：

- **backend 抽象**：驱动中立的接口 + scheme 注册表，ODBC 迁到接口之后、改为
  `PRIVATE` 链接。`UNIORM_BACKEND_ODBC=OFF` 的构建编得出核心库、跑得了它自己的
  单测，链接行里没有 ODBC。
- **自省成为公开契约**：`orm::schema()` 交出 `uniorm::schema_meta`，目录读取退到
  拥有它的 backend 里面。生成器建在消费者同样会用的那对 `orm`/`schema()` 之上，
  于是它既不点名驱动、也不链接 `find_package` 交出来的东西以下的任何目标——它的
  配置与输出用例随之搬进不链 ODBC 的那个测试目标。
- **标识符拼法成为部署策略**：`dialect::identifier_case` 由连接持有，只在标识符进
  SQL 的那一处生效，`validate()` 按同一个策略去问目录。默认 `keep`，不声明的人行为
  一字不变；另两折让一份生成物供得起把同一张表存成两种拼法的两家。落空的消息带出目录
  里的拼法，策略因此可以从错误里读出来。
- **不必写某一家的串也能连**：`connection_spec` 把 scheme、target、user、
  password 交给拥有这个 scheme 的 backend——只有它知道这些该怎么拼。串仍是
  每家都要实现的原语；不接四件套的 backend 就答 `capability_not_supported`，
  和这里其余可选能力一个形状。
- **同一个契约之上的异步表面**：`async.hpp` 把同步调用放到 worker 池上跑，按会话
  排先后，不动 backend 契约，也不动发出去的 SQL 一个字节。它的机制本身不需要库就
  能验（假 backend 上的单测），对着活库则集成用例拿同一份夹具从两侧各读一遍、逐
  字段相比，"一条通路"是比出来的，不是假设来的。做成仿真不是偷懒，是驱动
  没别的可拿：连接器把 `SQL_ASYNC_MODE` 报成 `SQL_AM_NONE`，ODBC 3.8 那两个
  异步信息字被拒，驱动管理器里根本没有 `SQLCompleteAsync`。哪一家 backend 真有
  原生异步，就把这一层换掉，调用方一行不改。

### CI 守什么

四支作业。`core` 把无 ODBC 的构建压在一对读下去只会报错的 `sql.h`/`sqlext.h`
上编，驱动类型一旦漏进公共头，就在这次专为它设的编译里失败。四支作业还各自把
每个安装头单独编成一个翻译单元（`uniorm_header_check`，只编不链），公共头若
要靠邻居先 include 才过得去，红的是这次构建而不是消费者的工程。
`driver` 带三条腿，
每条拿自家的连接器连自家会用的服务端，且每条在构建之前先问一句 DSN 背后答的是
哪台服务端，与自家名字不符即红——问哪句按腿给，PostgreSQL 问
`SHOW SERVER_VERSION`，因为那里 `SELECT VERSION()` 开头是名字不是数字。任何一条
测试被跳过都会让作业红，所以"五条全过"意味着确实连上了服务端。第四支是
`oracle-oci`：同一条规矩，但环里没有驱动管理器——它装 Oracle 的客户端库，按
scheme 名加载 backend，再对一只跑 Oracle Free 的容器跑同一套集成测试。它注册
四条 ctest 用例，连接器腿是五条——没有 ODBC 层可单测，也没有性能用例要跑。
三支腿在 runner 上已全绿：`core` 一条，每条腿五条，服务端依次答 8.4.11、
`11.8.9-MariaDB-ubu2404` 与 17.11。Oracle 那一支的装客户端与跑测试都在 runner
之外对活库重放过，还没被确认的是 runner 自己那一半：镜像拉取、首次启动在健康
门后面要多久、以及它那套 Ubuntu 里 libaio 叫什么。

### 两处代码学会去问的答案

**生成器读的是服务端答的元数据，不是方言写的 SQL。** 不是因为几家写的 SQL 不同
（`dialect::detect` 给两个 MySQL 线的 banner 同一套引号与分页，给 PostgreSQL 的
banner ANSI 那套默认），而是因为生成器走的那张目录是服务端的答案：MariaDB 连接器
`3.1.12` 用 `COLUMN_KEY = 'pri'` 去问 `information_schema`，撞上 MySQL 8.4 把该列
声明成 `utf8mb3_bin` 而什么都问不到，生成的头文件主键整列消失，却照样编译、注册、
过 `validate()`。这类沉默如今由比对本身兜，三条腿都这么比：拿生成的代码与 golden
的比，两边每行 `//` 之后的注释先截掉——注释按"连接器 × 服务端"每格都不同，留着它
就等于把 golden 钉死在一格上。那次主键读丢在代码里就是 `.column` 撞上 golden 的
`.primary_key`；只有 FK 与二级索引还要靠标记点名，因为它们在代码里不留任何痕迹。

**返回值也可以是驱动的意见，而不是服务端的事实**：数组绑定的 UPDATE/DELETE，
psqlODBC 每组参数都执行了，`SQLRowCount` 却停在其中一组的行数，两支 MySQL 线
连接器报的是整组的总数。`update()` 与 `remove()` 返回的正是这个数，于是核心改向
backend 要 `array_rowcount_totals`，没有它就一组一次 execute 地扫。

### 未结

跨格配法——MariaDB 连接器对 MySQL 服务端、Connector/ODBC 对 MariaDB 服务端——
不在 CI 里，这是选的不是漏的：作业守的是一支驱动实际会与哪家同配的那种用法。
设计文档 §9 把它记成一笔认下的缺口，而且值得记：迄今唯一那次真读丢，正是从跨格
撞出来的。

后续为 libpq / Oracle OCI 原生 backend 等，见设计文档 §5 与 §9。

## 许可证

MIT，见 [LICENSE](LICENSE)。
