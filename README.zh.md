# uniorm

[English](README.md)

现代 C++20 数据库访问层。核心是驱动中立的：连接串里的 scheme 选择 backend，
而目前落地实现的 backend 只有 ODBC 一个——也正因如此，uniorm 能访问任意提供
ODBC 驱动的数据库，而不依赖特定厂商的 C 客户端。实体映射、成员指针查询构建、
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
  - 实体映射：注册表 + 类型安全的成员指针谓词构建器 `db.query().of<T>()`
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
- **连接池**：懒创建、借还超时；所有池共用一条全局维护线程执行心跳保活与
  空闲超时回收
- **活库 schema 自省**：`orm::schema()` 与 `connection::schema()` 交出一个
  `uniorm::schema_meta`：库名、表清单，以及每张表的列、主键、外键与索引。
  `shape()` 把一张表约成 `column_shape` 列表——`validate()` 校验映射时读的正是
  它——于是生成器与校验器走同一份契约
- **方言自适应**：标识符引号与分页语法按 `SQL_DBMS_NAME` 推断
  （MySQL/MariaDB 用反引号 + LIMIT/OFFSET，其余 ANSI）
- **代码生成**：`uniorm-gen` 连活库，经 backend 自省读取 schema，生成实体
  struct + 注册函数（TOML 覆写类型/类名/跳过表）
- **可插拔 backend**：核心 API 构建在驱动中立的 backend 接口之上，连接串
  scheme 选择后端（`odbc://...`；裸 ODBC 连接串保持向后兼容），而目前落地实现
  的后端只有 ODBC 一个。目录读取也是这份接口的一部分（`schema()`），没有目录
  的 backend 于是答 `capability_not_supported`，而不是在某次读取内部失败；
  能力按后端声明，但目前被读的只有两个：`columnar_batch` 选批量写的那条通道，
  `array_rowcount_totals` 决定数受累行数的扫描怎么分批

## 要求

- C++20 编译器——构建只要求 `cxx_std_20`，不编码任何版本下限；CI 用
  Ubuntu 24.04 的默认 GCC 编译
- CMake ≥ 3.20
- unixODBC（`find_package(ODBC)`）及目标数据库的 ODBC 驱动——仅在
  `UNIORM_BACKEND_ODBC=ON` 时需要

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

产物为动态库 `libuniorm.so`（头文件在 `include/uniorm/`）：目标一律是
`SHARED`，`BUILD_SHARED_LIBS` 改变不了它。`VERSION` 取工程版本（0.2.0），
`SOVERSION` 取 major.minor（0.2），这是刻意的——拿着旧二进制的消费者会在链接期
失败，而不是等到运行时。

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
auto adults = db.query()
                .of<User>()
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
opts.connection_string = "DSN=mydb;UID=user;PWD=secret";
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

### 代码生成（uniorm-gen）

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
```

`--dsn` 与 `--connection-string` 是同一件事的两种写法——前者拼成
`DSN=<dsn>[;UID=…][;PWD=…]`，交给消费者自己也会用的那个 `uniorm::orm`
构造器——两者只能选一个，`--out` 则必须给。`--catalog` 与 `--schema`
收窄去哪找表（留空即由连接说了算），`--name` 改输出单元名，`--tables`
取逗号分隔的表名列表。

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

覆写仅限注册表可绑定的类型，完整规格见设计文档 §6。

## 测试

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**：纯内存测试，无外部依赖；基于 fake backend 运行且不链接
  ODBC，公共 API 一旦泄漏驱动类型即编译失败。`UNIORM_BUILD_TOOLS=ON` 时它还
  收纳生成器的配置与输出用例——那部分逻辑已不点名任何驱动，于是住进那个不链
  驱动的测试目标
- **odbc_unit_tests**：私有 ODBC 句柄层及其错误转换（仅驱动管理器，无需
  DSN）；只在 `UNIORM_BACKEND_ODBC=ON` 时构建
- **integration_tests**：需要可达的 ODBC DSN，读取环境变量
  `UNIORM_IT_DSN`、`UNIORM_IT_USER`、`UNIORM_IT_PWD`（三者均须设置），
  凭据以 `UID`/`PWD` 拼入连接串；任一未设置或连不上时以 ctest SKIP 处理
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
src/                  实现（构建为 libuniorm.so）；私有头贴着对应 .cpp 存放，
                        以相对名引用，不在任何对外 include 路径上
  backend/            scheme 解析与 backend 注册表
  odbc/               ODBC backend（适配器、句柄 RAII 封装、错误，以及应答
                        schema() 的目录读取）
                        全仓库唯一出现 <sql.h> 之处
  statement_cache.hpp 预编译语句 LRU 缓存
  orm_mapping.hpp     实体写入按什么匹配、赋哪些列——只从映射推导，不碰连接
cmake/                装出去的 find_package 所读的包配置模板
tools/uniorm-gen      代码生成 CLI（schema 提取 + TOML 配置 + 生成器）
tests/unit            单元测试
tests/integration     数据库集成测试（含 uniorm-gen 的 golden 头文件）
tests/install         外部消费者工程（install_smoke 用它验证安装面）
tests/perf            性能基准测试
docs/design.md        设计文档（权威 API 参考）
```

## 状态

v1 已完成并通过 MariaDB 集成验证（含 `uniorm-gen` 端到端）；同一套测试经
Connector/ODBC 走服务端预处理，对一个真 MySQL 服务端也全绿，经 psqlODBC 对一个
真 PostgreSQL 17 服务端同样全绿。v2 进行中：
backend 抽象已落地（中立接口 + scheme 注册表，ODBC 迁移至接口之后、
改为 PRIVATE 链接，核心单测在不链接 ODBC 的情况下编译运行），v1 最后一笔
类型层面的欠账已清（`uniorm::decimal_t`），自省所读的 schema 也成了一
份独立的公开契约：`orm::schema()` 交出 `uniorm::schema_meta`，目录读取退到
拥有它的 backend 里面。生成器建在消费者同样会用的那对
`orm`/`schema()` 之上，于是它既不点名驱动、也不链接 `find_package` 交出来的
东西以下的任何目标——生成器的配置与输出用例因此搬进了不链 ODBC 的那个测试目标。
CI 工作流已入仓库，两条形状
都有人守（不链接 ODBC 的编译契约 + 每条腿拿自家连接器连自家服务端、对活库跑）。
加服务端这一轴，不是因为几家写的 SQL 都不同（`dialect::detect` 给两个 MySQL 线的
banner 同一套引号与分页，给 PostgreSQL 的 banner ANSI 那套默认），而是因为生成器读的是
服务端答的元数据：本地第一次拿真 MySQL 8.4 跑，
就撞出 MariaDB 连接器 `3.1.12` 用 `COLUMN_KEY = 'pri'` 去问 `information_schema`，
撞上那台服务端把该列声明成 `utf8mb3_bin` 而什么都问不到，生成的头文件主键整列消失，
却照样编译、注册、过 `validate(strict)`。这类沉默如今由比对本身兜：抽取测试拿生成的
代码与 golden 的比，两边每行 `//` 之后的注释先截掉，三条腿都这么比——注释按"连接器
× 服务端"每格都不同，留着它就等于把 golden 钉死在一格上。那次主键读丢在代码里就是
`.column` 撞上 golden 的 `.primary_key`；只有 FK 与二级索引还要靠标记点名，因为
它们在代码里不留任何痕迹。GitHub 上跑绿过一副两条腿的形状
（两条腿当时都连 MariaDB 服务端；其中一趟还把一处驱动头漏进无 ODBC 构建的地方抓了
出来，本地重放抓不到它——仿 runner 的那只容器为了编驱动早就装好了 ODBC 开发头）。
那两条腿后来沿服务端铺开成四条，在装着 CI 钉的两支连接器的 `ubuntu:24.04` 容器里
对 `8.4.11` 与 `11.8.9` 各连一次跑满四格、五条测试条条全绿，随后又收回成两条同名配对的
形状：CI 守的是正常用法，跨格那两种配法是有意识地拿掉的，§9 里留着这笔账。
如今这副上长出第三条腿——psqlODBC 对 PostgreSQL 17，三支里唯一 Ubuntu 自己打包的那支——
而它撞出来的那处只有真连 PG 才撞得到：数组绑定的 UPDATE/DELETE，psqlODBC 每组参数都
执行了，`SQLRowCount` 却停在其中一组的行数，两支 MySQL 线连接器报的是整组的总数。
`update()` 与 `remove()` 返回的正是这个数，于是核心改问 `array_rowcount_totals`，
没有它就一组一次 execute 地扫。夹具表改成三家都认的写法，一份 golden 便供三家。
每条腿在构建之前还要问一句 DSN 背后答的是哪台服务端，与自家名字不符即红——问哪句也是
按腿给的，PostgreSQL 的 `SELECT VERSION()` 开头是名字不是数字，那条腿问
`SHOW SERVER_VERSION`。这副四支作业的形状都在 runner 的同族镜像里按作业原样重放过，
还欠 runner 一趟。后续为 libpq / Oracle OCI 原生 backend 等，见设计文档 §5 与 §9。
