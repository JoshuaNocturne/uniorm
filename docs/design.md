# uniorm v1 设计文档

状态：v1 实现完成；v2 进行中——backend 抽象、plugin 化、逐名解析、
`connection_spec`、异步表面、Oracle OCI 与 libpq 那两家都已落地（§5、§9）
日期：2026-10-29（本版按当前代码逐节核对，偏差集中记于"已知缺口"——该节现已为空）

## 1. 目标与范围

uniorm 是一个现代化的 C++ 数据库访问层：核心构建在驱动中立的 backend 接口之上
（§5.2）。v1 落地的 backend 只有 ODBC 一个，而不是各数据库的专有 C 客户端；v2
之上按同一形状长出第二家 Oracle OCI 与第三家 libpq，三家都是独立 plugin（§5.4）。

### v1 范围（In Scope）

- ODBC 句柄的 RAII 封装（env / dbc / stmt）
- 连接管理与最小连接池（固定大小、借还、超时）
- 事务（RAII）
- 参数绑定、语句执行、结果集迭代
- 批量 CRUD：数组参数绑定（`SQL_ATTR_PARAMSET_SIZE` 分批）+ 块取行
  （`SQL_ATTR_ROW_ARRAY_SIZE`），见 §4.5.2
- 类型系统：SQL 类型 ↔ 现代 C++ 类型映射，含 converter 扩展点（§4.4）
- 三种结果对象：
  - 聚合 struct 投影（按列序绑定，无需注册；注册过的类型改按列名，见 §4.6）
  - 实体映射（成员指针路线，显式注册）
  - 动态行对象（按列名取 variant 值）
- 基于成员指针的类型安全查询构建器
- Schema 校验（经 `orm::schema()` 读表元数据）
- `uniorm-gen` 代码生成工具（活连接或 DDL 脚本、schema 自省、TOML 配置）

### v1 明确不做（Out of Scope）

- Unit of Work / 脏检查 / 级联加载 / 懒加载（留待 v2）
- 异步流式游标与原生异步 backend（异步表面已落在 `async.hpp`，是同步契约之上的
  仿真，理由见 §4.11；驱动侧的原生异步要等拥有它的 backend）
- 游标更新（可更新结果集游标；批量写入走数组参数绑定，见 §4.5.2）
- Schema 迁移管理（migration）

### 已知缺口（v1 声明未接线 / 实现留有欠账）

本节记设计契约与实现之间的偏差，列在此处而非埋在正文，是为了让"文档承诺 =
当前实现"这条约束成立（§3.1/§4.2/§4.3/§10 已就地标注）。

**本节现为空。**原先只剩的 DECIMAL 一条已两头闭合：动态行的无损读取
（`slot_kind::text` + `SQL_C_CHAR`，`value_cast` 按需解析，`column_info::scale`）
与它上层欠的 `uniorm::decimal_t` 都在实现里了，行为契约见 §4.3，了结过程见 §9。
本节更早的一条"ODBC 宽字符路径未使用"也已按它自己写下的处置意见了结：确认不做，
删掉零调用者的实现（§4.2）。

另有一处较小的偏差体量不足以单列，直接在正文就地写实并进了 §9：先前"不碰 ODBC"
只到链接行为止，运行时 `libuniorm.so` 自己仍会拽住驱动管理器；backend 拆分已把这条
边界推到运行时（§5.1）。

### 基础决策

| 决策项 | 结论 |
|---|---|
| C++ 标准 | C++20 |
| 接口风格 | 同步为主，异步是可选表面（`async.hpp`，§4.11）；两者共用同一条 backend 契约，异步侧不新增数据库通路 |
| 平台 | Linux（unixODBC）、Windows（原生 ODBC） |
| 错误处理 | 异常 |
| Unicode | 内部一律 UTF-8；只做窄字符绑定（`SQL_C_CHAR`），编码转换留在驱动侧（§4.2） |
| 库形态 | 动态库（`libuniorm.so` / `uniorm.dll`），经 `UNIORM_API` 导出宏控制符号可见性；每家 backend 一份独立动态库（当前 `libuniorm_odbc.so` / `uniorm_odbc.dll`，走 `UNIORM_ODBC_API`），核心不链任何驱动（§5.1）；模板密集代码（mapping/builder/projection/pfr）保留在头文件 |

## 2. 分层架构

```
┌────────────────────────────────────────────────┐
│ 查询构建器 query<T>   实体映射 mapping registry │  高层 API
│ 聚合投影 projection   动态行 row                │
├────────────────────────────────────────────────┤
│ connection / connection_pool / transaction      │  连接层
├────────────────────────────────────────────────┤
│ statement：参数绑定 / 执行 / result_set 迭代     │  语句层
├────────────────────────────────────────────────┤
│ backend::statement_iface / connection_iface     │  backend 抽象（v2 M1，见 §5）
│ scheme 注册表 / capabilities / 中立列缓冲        │
├────────────────────────────────────────────────┤
│ environment / connection / statement 句柄 RAII   │  ODBC backend
│ odbc_error / diagnostics                        │  （plugin 之一，另一家 §5.4）
└────────────────────────────────────────────────┘
```

依赖方向严格向下；高层不直接触碰 `SQLH*` 句柄类型。ODBC 专有概念（`SQLLEN` / indicator / SQLSTATE / `?` 占位符细节）禁止上浮到语句层公共 API 之上——该纪律由不链 ODBC 的核心单测目标在**编译与链接期**把守；驱动本身也只由 `libuniorm_odbc.so` 一家映射进进程，核心 `libuniorm.so` 的 `DT_NEEDED` 里没有 `libodbc`（边界见 §5.1）。

## 3. 目录结构

```
uniorm/
├── CMakeLists.txt               # 库目标 + 安装/导出规则（仅 top-level 时生效）
├── cmake/uniormConfig.cmake.in  # find_package(uniorm CONFIG) 的包配置模板
├── include/uniorm/              # 对外 public 头文件（消费者唯一的 include 根；
│                                #  安装时整目录搬到 <prefix>/include/uniorm）
│   ├── uniorm.hpp               # umbrella：README 示例只需它
│   ├── export.hpp               # UNIORM_API 符号导出宏
│   ├── error.hpp                # 异常体系（backend/odbc 层错误在各自头文件）
│   ├── value.hpp                # sql_value variant / timestamp
│   ├── types.hpp                # backend 中立 sql_type 枚举 + sql_type_name + column_info
│   ├── converter.hpp            # 自定义类型转换器（concept has_converter，§4.4）
│   ├── decimal.hpp              # decimal_t 精确固定点小数值 + 其 converter 特化（§4.3）
│   ├── row.hpp                  # 动态行 + value_cast
│   ├── params.hpp               # 参数容器 + make_sql_value 转换
│   ├── result_set.hpp           # 结果集游标（pimpl，块取行 + 逐行物化 row）
│   ├── schema.hpp               # column_shape / schema_meta：活库表结构
│   │                            # 的读取契约（见 §5.2）
│   ├── connection.hpp           # connection：连接前置的 low-level 入口（含语句缓存原语）
│   ├── connection_spec.hpp      # 四件套：scheme / target / user / password
│   ├── transaction.hpp
│   ├── pool.hpp                 # connection_pool / pooled_connection / connection_pool_registry
│   ├── dialect.hpp              # 方言特性（引用符、标识符拼法、分页）
│   ├── async.hpp                # 异步表面（§4.11）：executor / task<T>
│   │                            #  / async::orm / async::transaction，
│   │                            #  每会话的 FIFO 也在这层（只有声明）
│   ├── backend/                 # 驱动中立的 backend 契约（见 §5.2）
│   │   ├── backend.hpp          # column_buffer / capabilities / statement_iface / connection_iface
│   │   │                        # / batch_writer_iface（列式批量写）
│   │   ├── registry.hpp         # scheme 解析 + backend 注册表（out-of-tree backend 注册入口）
│   │   └── error.hpp            # backend_error / capability_not_supported / unknown_scheme
│   ├── detail/
│   │   ├── entity_binding.hpp   # 注册实体的结果列直绑（按列名定位序号）
│   │   ├── fetch_rows.hpp       # 实体读与投影读共用的块读循环与取一行
│   │   ├── pfr.hpp              # 自实现聚合体反射（字段数探测 + 展开，上限 64）
│   │   ├── projection.hpp       # 聚合 struct 投影绑定（field_binding 体系）
│   │   ├── traits.hpp           # is_optional_v 等共享 traits
│   │   └── time.hpp             # chrono ↔ 日历拆分/组装
│   ├── orm.hpp                  # 实体注册与 CRUD 入口：除模板外只有声明，实现全在 src/orm.cpp
│   ├── mapping/registry.hpp     # 实体映射注册表（含 mapping_builder；生成头三依赖之一）
│   └── builder/
│       ├── builder.hpp          # query<T> / update<T> / remove<T> / update_builder / remove_builder
│       └── expression.hpp       # member_key / predicate / 谓词构造器
├── src/                         # 核心实现（编译进 libuniorm）；私有头贴着 .cpp 存放
│   ├── identifier.hpp           # 全库唯一的那一处 ASCII 折叠：凡比较或发出
│   │                            # 标识符名字的都经它（§4.8、§5.2、§6.2）；
│   │                            # 核心、backend 与生成器共读
│   ├── orm_mapping.hpp          # 实体写的 WHERE 解析与 SET/WHERE 列划分（纯映射规则）
│   ├── statement_cache.hpp      # LRU 预编译语句缓存（存 statement_iface，容量固定 64）
│   ├── async/                   # 异步表面的函数体：async.cpp 装会话与句柄、
│   │                            #  那条 FIFO 与 executor 的虚析构，
│   │                            #  worker_pool.cpp 装默认 executor 的线程池
│   └── backend/                 # scheme 解析与注册表实现（含 plugin_loader.cpp：
│                                #  按 scheme 从 <libdir>/uniorm/backends dlopen）
├── backends/                    # 每家 backend 一个独立 shared library，与 src/
│                                #  同级：链接形状与"是不是核心的一部分"一眼可辨
│   ├── odbc/                    # libuniorm_odbc.so，导出 plugin 契约
│   │   ├── CMakeLists.txt       # 独立 shared 目标，PRIVATE 链 ODBC::ODBC，
│   │   │                        #  装到 <libdir>/uniorm/backends/
│   │   ├── export.hpp           # 私有 UNIORM_ODBC_API 宏（跨 DSO 的可见性）
│   │   ├── backend.hpp          # backend 契约的 ODBC 实现（含 odbc_batch_writer）
│   │   ├── environment.hpp / connection.hpp / statement.hpp   # 句柄 RAII
│   │   ├── handles.hpp          # 句柄 RAII 模板、traits
│   │   ├── native_types.hpp     # SQL_* → sql_type 映射表（驱动编码只活在这里）
│   │   ├── schema_catalog.hpp/.cpp   # uniorm::schema_meta 的 ODBC 实现
│   │   └── error.hpp            # odbc_error / diagnostics
│   └── oracle_oci/              # libuniorm_oracle.so（scheme "oracle"）：plugin
│                                #  契约的第二家，同一组句柄 RAII，外加 `?` 到
│                                #  `:n` 的改写（sql_text）与它自己的类型表；
│                                #  不链任何 ODBC，目录读取尚未答（§5.4）
├── tools/uniorm-gen/            # 代码生成：uniorm_gen_core(STATIC) + uniorm-gen(CLI)
│   ├── main.cpp                 # 参数解析与编排（唯一进 CLI 的源文件）
│   ├── schema_reader.cpp/.hpp   # backend 自省 → schema 模型（筛选/分组在这层）
│   ├── generator.cpp/.hpp       # model + 配置 → 头文件文本
│   ├── config.cpp/.hpp          # TOML 子集解析
│   ├── naming.cpp/.hpp          # PascalCase/camelCase 标识符转换
│   └── schema_model.hpp         # 中间 schema 模型（生成器输入；列类型已是中立 sql_type）
├── tests/
│   ├── unit/                    # 无库依赖：check.hpp（CHECK/CHECK_THROWS）+
│   │                            # uniorm_unit_tests（不链 ODBC，含 fake backend 的
│   │                            # test_pool.cpp、fake 目录的
│   │                            # test_orm_validate.cpp、异步的
│   │                            # test_async_task.cpp 与 fake 会话的
│   │                            # test_async_orm.cpp，另加 uniorm-gen 的
│   │                            # 纯逻辑用例）与
│   │                            # uniorm_odbc_unit_tests（句柄 RAII、错误派生、
│   │                            # 目录行再筛的判据）、uniorm_oracle_oci_unit_tests
│   │                            # （占位符改写、它自己的类型表、连接串怎么切分）
│   ├── integration/             # 需活连接：test_integration.cpp 一份源码出两个
│   │                            # ctest 名字，UNIORM_IT_BACKEND 决定它走 ODBC 还是
│   │                            # 按 scheme 载 Oracle plugin；另有
│   │                            # test_gen_e2e.cpp 与 golden/gen_it_schema.hpp
│   │                            # （生成物快照）
│   ├── install/                 # 外部消费者工程（install_smoke 用它走 find_package）
│   ├── install_smoke.cmake.in   # 安装面冒烟脚本（cmake -P）
│   └── perf/                    # test_perf.cpp（ctest 标签 perf）
├── docs/design.md
├── docs/proposal-async-api.md   # 异步表面的提案：取舍与分步落地
├── docs/async-sequence.md       # 异步表面的时序图：类型怎么搭手
├── README.md / README.zh.md     # 双语入口文档
├── .clang-format                # Google 风格基线（未挂 hook，流水线也不校验格式）
├── .github/workflows/ci.yml     # 编译契约（无 ODBC）+ backend 对活库跑 ctest
└── .gitignore
```

公开/私有边界按"外部消费者是否需要"判定：凡出现在 `uniorm/uniorm.hpp` 或
生成头所依赖的 `uniorm/orm.hpp` / `uniorm/mapping/registry.hpp` /
`uniorm/decimal.hpp` 传递闭包内的头文件留在 `include/`，
其余下沉：核心的私有头与实现贴邻放在 `src/`，backend 的私有头贴邻放在 `backends/<scheme>/`，
都用引号相对名互相引用。于是 `src/` 不在 `uniorm` 目标的任何 include 路径上（同目录
引用无需路径），只有跨目录点名核心私有头的目标显式带 `-I src`。
`uniorm_unit_tests` 读 `src/orm_mapping.hpp` 一类；那唯一一处 ASCII
折叠 `src/identifier.hpp` 由核心、ODBC backend 与生成器共读，所以
`uniorm_odbc`、`uniorm_gen_core` 与 `uniorm_integration_tests` 也带上它
——折叠只能有一个来源，而它既不是公开头，也不属于任何一家的目录。
backend 侧的 `uniorm_odbc_unit_tests` 另带 `-I backends` 读句柄层。
`uniorm-gen` 曾在 `-I` 之列是为了 `SQLTables` / `SQLColumns` 的声明；
那些读取挪进 ODBC backend 后，驱动那侧回到了公开边界，剩下的只有这把折叠。
公开头一旦 `#include` 私有头便无法解析，边界由编译器强制；库内的 `<sql.h>`
只出现在 `backends/odbc/` 之下，对外头文件既不带驱动类型，也不带 ODBC 链接依赖
（`ODBC::ODBC` 是 PRIVATE）。反向的边界同样由编译器强制：每个安装头各自编成
一个翻译单元（`uniorm_header_check`，只编不链），公开头不能靠邻居先 include
才过得去。

公开头只留声明：非模板成员的定义一律进同名 `.cpp`（`orm.cpp`、`decimal.cpp`、
`async.cpp` 都按这条走）。定义搬出类外时导出标记不会跟着走——类外的
`operator` 友元要在声明上自己写一次 `UNIORM_API`，`converter` 的显式特化则把
标记写在整个特化上。

### 3.1 安装与集成

`cmake --install build --prefix <p>` 产出四类文件（目录名取自 `GNUInstallDirs`，
64 位 RHEL/Fedora 上 `<libdir>` 解析为 `lib64`）：

| 位置 | 内容 |
|---|---|
| `<libdir>/` | `libuniorm.so.<VERSION>` 加 `SOVERSION`（`0.4`）与裸名两级符号链接；Windows 下 DLL 走 RUNTIME、导入库走 ARCHIVE |
| `<libdir>/uniorm/backends/` | 每家启用的 backend 一份 `libuniorm_<scheme>.so.<VERSION>`（当前 `libuniorm_odbc.so.<VERSION>`）；核心按 scheme 在此 dlopen（见 §5.1） |
| `include/uniorm/` | 全部 public 头文件；私有头贴邻 `.cpp` 留在 `src/`，不参与安装 |
| `<libdir>/cmake/uniorm/` | `uniormConfig.cmake`、`uniormConfigVersion.cmake`、`uniormTargets.cmake` 与 `uniormTargets-<config>.cmake`；`uniormOdbcTargets*.cmake` 只为仓内白盒测试保留，消费者不再按组件取用 |
| `<bindir>/uniorm-gen` | 代码生成 CLI；仅 `UNIORM_BUILD_TOOLS=ON` 时安装（`UNIORM_BACKEND_ODBC=OFF` 时该选项被 CMake 直接拦下） |

消费者 `find_package(uniorm REQUIRED CONFIG)` 后链接 `uniorm::uniorm`，include 路径与
C++20 标准都由导出目标携带：公开头自己就要用 `concept` 和 `remove_cvref_t`，而
`CMAKE_CXX_STANDARD` 只作用于本工程，所以 `cxx_std_20` 走 `PUBLIC` 编译特性而不是留给
消费者猜。四条约定：

- 安装块整体包在 `if(PROJECT_IS_TOP_LEVEL)` 里：`add_subdirectory` / FetchContent
  集成只拿到目标，不会把本项目的安装规则带进宿主的 `install`；
- `SOVERSION` 取 `major.minor`，包版本兼容取 `SameMinorVersion`。0.x 没有 ABI 承诺
  可守——给 `connection` 加一个成员就足以让已编译的消费者崩在运行期——与其用
  `libuniorm.so.0` 掩盖这种破坏，不如让链接期直接失败；
- ODBC 与线程对核心都是 PRIVATE 依赖，不进核心的导出接口；且核心 `.so` 根本不链
  ODBC，`libodbc.so.2` 的 `DT_NEEDED` 落在 `libuniorm_odbc.so` 上。消费者不需要
  `find_dependency(ODBC)`，也不必为链接行了结驱动；消费者的链接行始终只有
  `uniorm::uniorm`，用哪家 backend 由运行时按 scheme 到 `<libdir>/uniorm/backends/`
  下 dlopen 决定（见 §5.1）。
- `uniorm-gen` 走 RUNTIME 安装但不进 `EXPORT`：它是"跑一遍"的程序，不是被链接的
  目标，导出它便等于把 `uniorm_gen_core`（内部静态切分，靠 `-I src` 读私有头）
  伪装成对外 API。它的 `DT_NEEDED` 写死 `libuniorm.so.<SOVERSION>`（现为 `0.4`）
  与 `libuniorm_odbc.so.<SOVERSION>`——工具仍按白盒路径直链 backend 拿自省——而
  构建树留下的 `RPATH` 是绝对路径，故 ELF 上以 `INSTALL_RPATH` 改写成
  `$ORIGIN/../<libdir>;$ORIGIN/../<libdir>/uniorm/backends`——装到哪个 prefix
  就找哪个 prefix，与库同树发布时版本必然对上。

导出的 CMake 文件不含绝对前缀：`DESTDIR=<stage> cmake --install build --prefix
/usr/local` 能打到暂存根再整体搬迁（实测装出来的 4 个 CMake 文件里既无 stage 也
无 prefix 的字面量）。暂存根下的 `uniorm-gen` 也能直接跑起来——`$ORIGIN` 相对路径
要买的就是这一点。

上面这些如今由 `install_smoke`（§8）把关：装进构建树下的临时 prefix，比对装出来的
头文件集合与 `include/` 一致，核对 `<libdir>/uniorm/backends/` 下 plugin 文件随
`UNIORM_BACKEND_ODBC` 该有则有、该无则无，用一个刻意不设 `CMAKE_CXX_STANDARD`
的外部工程 `find_package` + 编译 + 运行，再以 `UNIORM_PLUGIN_PATH` env 把 loader
指到本次装的 plugin 上，跑通"裸 DSN 载到 ODBC 层"或"无 plugin 时保持
`unknown_scheme`"，故意以 `99.0.0` 配置一次要求被拒，最后跑装出来的
`uniorm-gen --help` 证明 `$ORIGIN/../<libdir>` 与 `$ORIGIN/../<libdir>/uniorm/backends`
都真的载到了同 prefix 的库。

## 4. 核心模块设计

### 4.1 ODBC 封装层

RAII、move-only 的句柄包装，屏蔽所有 `SQLFreeHandle` / 错误提取细节。最终 API：

```cpp
namespace uniorm::odbc {

class odbc_error : public backend::backend_error {   // backend 名固定 "odbc"
public:
    using diagnostic = backend::backend_error::diagnostic;  // {state, native_code, message}
    odbc_error(std::string const& context, std::vector<diagnostic> diags);
    // diagnostics() 继承自 backend_error
};

// 错误提取辅助（error.hpp）
std::vector<odbc_error::diagnostic>
collect_diagnostics(SQLSMALLINT handle_type, SQLHANDLE handle);
void throw_if_error(SQLRETURN rc, SQLSMALLINT handle_type, SQLHANDLE handle,
                    std::string const& context, bool tolerate_no_data = false);

class environment {                    // SQLHENV
    environment();                     // ODBC 3.80，失败回退 3.x
    SQLHENV native() const noexcept;
};
environment& shared_environment();     // 进程内共享，首次使用时创建

class connection {                     // SQLHDBC，move-only
    explicit connection(environment& env);
    void open(std::string_view connection_string);      // SQLDriverConnect
    void close();
    bool is_open() const noexcept;
    void set_autocommit(bool enabled);                  // 事务支持
    void commit();
    void rollback();
    SQLHDBC native() const noexcept;
};

class statement {                      // SQLHSTMT，move-only
    explicit statement(connection& conn);
    void prepare(std::string_view sql);
    void execute();                    // 容忍 SQL_NO_DATA
    bool fetch();                      // 绑定行数组时一次取一块；false = 结果耗尽
    std::size_t affected_rows() const;
    std::size_t column_count() const;
    // 块取行：每块行数与本轮实际取到的行数
    void set_row_array_size(SQLULEN size);      // SQL_ATTR_ROW_ARRAY_SIZE
    SQLULEN rows_fetched() const noexcept;      // SQL_ATTR_ROWS_FETCHED_PTR
    // 数组绑定：每次 SQLExecute 提交的参数集个数
    void set_paramset_size(SQLULEN size);       // SQL_ATTR_PARAMSET_SIZE
    // index 均为 1-based；indicator 指向调用方持有、生命周期覆盖执行的 SQLLEN
    void bind_parameter(SQLUSMALLINT index, SQLSMALLINT c_type, SQLSMALLINT sql_type,
                        SQLPOINTER value, SQLLEN buffer_length, SQLLEN* indicator,
                        SQLULEN column_size = 0, SQLSMALLINT decimal_digits = 0);
    void bind_column(SQLUSMALLINT index, SQLSMALLINT c_type, SQLPOINTER value,
                     SQLLEN buffer_length, SQLLEN* indicator);
    void close_cursor();
    // 复用前的复位：仅 SQLFreeStmt(SQL_CLOSE)（无游标时是 no-op，故缓存复用安全），
    // 并把 row array / paramset size 落回 1。不做 SQL_UNBIND / SQL_RESET_PARAMS——
    // SQLBindCol / SQLBindParameter 本身覆盖旧绑定。
    void reset();
    SQLHSTMT native() const noexcept;
};

} // namespace uniorm::odbc
```

方言推断不在本层：高层 `connection::dbms_name()`（`SQLGetInfo(SQL_DBMS_NAME)`）
输出产品名，交由 `dialect::detect(std::string_view)` 解释（见 §4.8），
保持 ODBC 封装层不依赖高层类型。

错误处理策略：所有 ODBC 调用检查返回码，非成功（`SQL_SUCCESS` / `SQL_SUCCESS_WITH_INFO` 之外）即提取 `SQLGetDiagRec` 全部记录后抛 `odbc_error`。`SQL_SUCCESS_WITH_INFO` 记录为诊断信息但不抛异常，可通过可选回调或日志钩子暴露。

关键实现细节：

- indicator buffer 一律用 `SQLLEN`，规避 32/64 位截断问题；
- 字符串列与参数一律按窄字符绑定（`SQL_C_CHAR` / `SQL_VARCHAR`），库内不做编码
  转换；WVARCHAR 等宽 SQL 类型能取到值，是驱动自己做了字符集转换（策略见 §4.2）；
- 长数据（长 VARCHAR / BLOB）v1 策略：绑定固定缓冲，截断（indicator 为负——`SQL_NO_TOTAL` 或 `SQL_NTS`——或超出缓冲）时经 `SQLGetData` 循环重取**完整值**整体替换——MariaDB Connector/ODBC 在截断续读时返回的是全量值而非剩余部分，追加式拼接会重复计数据；
- 实际设置的 ODBC 属性全集：`SQL_ATTR_ODBC_VERSION`（先试 `SQL_OV_ODBC3_80`，
  失败回退 `SQL_OV_ODBC3`）、`SQL_ATTR_AUTOCOMMIT`、`SQL_ATTR_ROW_ARRAY_SIZE`、
  `SQL_ATTR_ROWS_FETCHED_PTR`（构造语句时一次性绑定，故 `fetch()` 后直接读计数）、
  `SQL_ATTR_PARAMSET_SIZE`。刻意不设：`SQL_ATTR_PARAMS_BOUND`、
  `SQL_ATTR_PARAM_BIND_TYPE`（批量绑定按列主序构造——每列一段连续数组，stride 即
  `BufferLength`）、`SQL_ATTR_METADATA_ID`、游标/并发属性（无服务端游标、
  无游标更新）、`SQL_ATTR_ASYNC_ENABLE`（无异步——这一条现在有量过的数字：连接器
  把 `SQL_ASYNC_MODE` 报成 `SQL_AM_NONE`，`SQL_MAX_ASYNC_CONCURRENT_STATEMENTS`
  为 `0x0`，ODBC 3.8 那两个异步信息字回 `SQL_ERROR` + `HY096 Invalid information
  type`，把属性设上去也是静默改回默认（`01S02 Option value changed`，回读
  `0x0`，随后 `SELECT 1` 照旧同步跑完），而驱动管理器根本不导出
  `SQLCompleteAsync`。异步因此只能是同步契约之上的仿真，见 §4.11）。

### 4.2 Unicode 策略

- 库内部所有 `std::string` / `string_view` 均为 UTF-8；
- ODBC 边界一律窄字符：列与参数按 `SQL_C_CHAR` / `SQL_VARCHAR` 绑定，连接串按
  `SQLDriverConnect` 的窄接口提交，库内不做任何编码转换；
- 于是"边界上的字节就是 UTF-8"这一前提由驱动与驱动管理器的字符集设置兜住
  （unixODBC 会按应用 locale 做 iconv 转换，MariaDB Connector/ODBC 按连接字符集
  输出）。locale 或驱动字符集不是 UTF-8 时，非 ASCII 数据就会错位——这是本策略的
  前提，库无从自查，也不打算自查。

曾经存在过一条宽字符路径：`src/unicode.hpp` 的 `utf8_to_utf16` / `utf16_to_utf8`
与公开类型 `unicode_error`。它们从落地起就没有库内调用者——没有任何一处绑定会
用到 UTF-16，也没有任何一处能验证它们；设想中"驱动拒绝 `SQL_C_CHAR` 时回退宽
字符"更是从来没有判定点。零调用者的实现留着，只会让文档与代码互相印证出一个不
存在的特性，故连唯一会抛出它们的 `unicode_error` 一并删除。

重新引入的触发条件：出现一个只暴露宽字符的 backend（Windows 上某些只支持 Unicode
驱动的 SQL Server 部署是典型例子）。届时需要一并带回三样东西——转换函数本身、
`SQL_C_WCHAR` 绑定路径，以及一个确实能观察到窄字符请求被拒或数据丢字的判定点；
少最后一样，就又只是一份没人调用的实现。

### 4.3 类型系统

`value.hpp` 定义动态值类型（动态行与元数据使用）：

```cpp
namespace uniorm {

using timestamp = std::chrono::system_clock::time_point;

using sql_value = std::variant<
    std::monostate,                  // NULL
    bool,
    int16_t, int32_t, int64_t,
    double,
    std::string,                     // UTF-8
    std::vector<std::byte>,
    timestamp>;

} // namespace uniorm
```

`value.hpp` 只有这两个别名与 `inline bool is_null(sql_value const&) noexcept;`；
取值转换 `value_cast<T>` 在 `row.hpp`，参数归一化 `detail::make_sql_value` 在
`params.hpp`。

映射不靠单一 traits，而是三条独立通道（下表是它们的合成结果）：
native code 归一为中立 `sql_type` 发生在 backend 之内（ODBC 是
`backends/odbc/native_types.hpp`，核心库的任何一处都不再出现驱动编码）；
动态行按 `sql_type` 选槽位种类（`src/result_set.cpp` 的 `kind_for`）；实体/投影侧由
成员类型决定绑哪种 `backend::buffer_type`（`readable_member` / `plain_sql_member`
concept 约束可声明的成员类型，`column_meta::buffer_type` 记录之），buffer_type 再在
ODBC adapter 内翻成 `SQL_C_*`；`uniorm-gen` 的默认映射决定生成实体用哪个 C++ 类型。

下表同时是 `orm::validate(strict)` 逐列执行的契约：成员（或其 converter 表示）可绑的
`sql_type` 集合以位掩码记在 `column_meta::accepted_types`，与活 schema 的列类型比对，
不匹配即 `mapping_error`；驱动报回无法归类的 `sql_type::other` 没有可比对的族，跳过。

| SQL 类型 | C++ 类型 |
|---|---|
| BIT | `bool` |
| TINYINT / SMALLINT / INTEGER / BIGINT | `int16_t` / `int32_t` / `int64_t`（`int8_t` 在 `plain_sql_member` 的名单里，但 `sql_value` 没有这一选项，成员用它编译不过，`uniorm-gen` 也拒作覆写类型） |
| REAL / FLOAT / DOUBLE | `double`（`float` 不是可绑定成员类型，单精度列一律落 `double`；`uniorm-gen` 对此告警） |
| DECIMAL / NUMERIC | 见下方"DECIMAL 策略"（实体/投影随字段类型：`std::string` 或 `decimal_t` 均无损；动态行取精确定点字面量，`get<std::string>` 无损、`get<decimal_t>` 解析成值、`get<double>` 按需解析） |
| CHAR / VARCHAR / LONGVARCHAR | `std::string` |
| WCHAR / WVARCHAR / WLONGVARCHAR | `std::string`（走窄字符路径，转换归驱动；库内未接线，见 §4.2） |
| DATE / TIME / TIMESTAMP | `timestamp`（DATE/TIME 补零时间部分后同样落为 `timestamp`；v1 不单独提供日历/时刻类型） |
| BINARY / VARBINARY / LONGVARBINARY | `std::vector<std::byte>` |
| GUID | `std::string`（v1 以字符串形式暴露） |

**DECIMAL 策略（默认无损，覆写按列）**：

1. **默认**：动态行一律按 `SQL_C_CHAR` 取精确定点字面量（`slot_kind::text`），
   `get<std::string>` 得字面量，`get<double>` 经 `value_cast` 解析（仍受 double
   精度所限）；`uniorm-gen` 把 DECIMAL/NUMERIC 列默认生成为 `std::string` 成员，
   实体/投影侧因此也按 `SQL_C_CHAR` 直绑。这个默认曾经可配（CMake 选项
   `UNIORM_DECIMAL_DEFAULT`，`string` / `double`），但库代码从不读它派生的宏，
   只有生成器的默认映射读，一个"只改生成物"的构建期旋钮不足以承担配置项的名义，
   故已删除；
2. **`uniorm::decimal_t`**（`include/uniorm/decimal.hpp`）：精确固定点值类型——
   `from_literal` 解析驱动字面量，尾数按十进制数字逐位存进定长数组（`max_digits`
   = 78，宽过 MariaDB `DECIMAL` 的 65 位上限），不额外分配一个字节，
   `to_literal` 还原读取时的样子（整数部分的前导零去掉，小数部分保留），
   `to_literal_into` 是同一渲染写进调用方槽位的形式（先 clear）——
   `converter<decimal_t>::to_db` 走这一支，于是长于短字符串缓冲区的列批量写时复用
   槽位容量而不是逐行分配（§4.4 的 `to_db` 契约）。
   比较与 `==` 按值而非字面量（`1.5` 与 `1.50` 相等），`scale()` 报的仍是字面量
   写出的位数；`to_double` 显式舍入，`to_int64` 在小数位非零或整数部分超范围时抛
   `type_mismatch`。有效位超过 78 的字面量同样抛 `type_mismatch`——那种列留给
   `std::string`。它**不是** `sql_value` 的新备选：动态行始终存字面量文本，
   `value_cast<decimal_t>` 走现成的 converter 分支。库不另开绑定路径——它到达
   实体/投影/参数三条通道全靠随附的 `converter<decimal_t>`（`db_type = std::string`），
   即与 §4.4 的第三方 decimal 类完全同构；
3. **逐列覆写**：要 `double`、`decimal_t` 或第三方 decimal 类，走生成器配置的
   `cpp_type` / `[types]`（§6.4，取值限于本节的可绑定集合），或 `converter<C>`
   特化（§4.4）——其 `db_type` 为 `std::string` 时即无损；`uniorm-gen` 侧的入口是
   `converter = "..."`，生成的成员就是该域类型，头文件以
   `static_assert(uniorm::has_converter<C>)` 要求特化存在。生成器的 DECIMAL 默认
   仍是 `std::string`（换默认会改动所有既有用户的生成物，而 `decimal_t` 只是个
   `cpp_type = "uniorm::decimal_t"` 的逐列开关）。

可空列对应 `std::optional<T>`；绑定与取值逻辑对 `optional` 做特化
（indicator 写 `backend::null_indicator`，其值即 ODBC 侧的 `SQL_NULL_DATA`）。

### 4.4 Converter（自定义类型扩展点）

一个域类型只有一种数据库表示：`converter<T>` 以嵌套 `db_type` 命名该表示，`to_db` /
`from_db` 负责双向换算。这两个名字都沿用既有词汇：形参就叫 `T`（库内命名，不叫 `Cpp`
——表示本身也是 C++ 类型，要分开的是两侧的类型，不是"是不是 C++"），别名不叫 `sql`
（本库里 `sql` 到处指渲染出的语句文本，会被读成"该类型的 SQL 文本"）。同一域类型需要
在两列上表示不同时，那是 schema 差异，解法是引入两个不同的域类型，而不是第二个特化。

```cpp
namespace uniorm {

template <class T>
struct converter {};                   // 用户特化：db_type / to_db / from_db

template <class T>
concept has_converter = /* 三者齐备 */;  // 缺任一项即视为无 converter

// 示例：enum ↔ 字符串
template <>
struct converter<Status> {
    using db_type = std::string;
    static void to_db(Status const& s, std::string& out);
    static Status from_db(std::string const& v);
};

} // namespace uniorm
```

库自带且只自带一支特化：`converter<decimal_t>`（§4.3 第 2 条）。它不在这个扩展点
之外另开路——下面列出的引用点就是它全部的接线处，库里没有第二条 decimal 路径。

`to_db` 赋值进调用方持有的槽位而不是返回一个值：表示长于短字符串缓冲区时，返回值
会让每次批量写入按行分配一次。它必须覆盖槽位，不能追加。

`from_db` 收到的是右值——调用方已经用完那个槽位。以带缓冲区的类型（如 `std::string`）
为表示的 converter 因此可以直接搬走它，而不是再拷一份；仍写 `db_type const&` 的特化
照样满足 `has_converter`，代价就是那次拷贝，只有 `db_type&` 形式会被拒。两种搬走的
写法不等价：形参写 `db_type&&` 比按值 `db_type` 少一次移动（按值形参本身就是先把槽位
移动一遍才开始解码），10000 行投影的配对测量里它稳定更便宜，每个被转换的文本列省数十
ns/row——该测量跨会话有约 ±20 ns/row 的漂移，所以这个数只当量级看。上例写 `const&`
是因为 enum 解码根本没有缓冲区可搬。

`has_converter` 的每个引用点，即该扩展点的接线范围：

- **值路径** `detail::make_sql_value`（`params.hpp`）：排在精确类型链之后、整型 /
  枚举 / 隐式转 `std::string` 三支之前，因此 converter 优先于那三支的静默降级。参数、
  查询构建器的比较值、`column_meta::read` 都走这一处；`row::get<T>`（`value_cast`，
  `row.hpp`）对称地用 `from_db` 解码；
- **读侧绑定** `detail::make_field_binding`（`detail/projection.hpp`）：
  `converter_binding` 按 `db_type` 绑 C 缓冲区，取到行后才 `from_db` 解码进字段——决定
  缓冲区的是表示，域类型本身不额外占一次拷贝；交出的槽位对本绑定已是死物，被搬走的话
  下一行重新暂存，那正是一个 `db_type` 类型字段本身也要付的分配次数。NULL 判定委托内层绑定
  （`indicator()` 是虚函数，组合绑定自己那张 indicator 数组不会被驱动写过）；
- **实体注册**（`mapping/registry.hpp`）：`readable_member` 接受带 converter 的成员，
  `column_meta::buffer_type` 与 `column_meta::accepted_types` 均由 `db_type` 推出；
- **批量写入** `columnar_batch_write`（`src/orm.cpp`）：表示先 `to_db` 再落进参数
  缓冲区，与普通成员走同一趟 memcpy。变长列的宽度预扫描量不出未编码的值，故每个
  converter 列在预扫描阶段多一次 `to_db`；
- **schema 校验** `orm::validate(strict)`：按 §4.3 的表比对列类型族。

`db_type` 本身必须是 §4.3 可绑定集合里的类型，否则 `member_buffer_type` 的
`static_assert` 在注册点即报错，不会退化成按文本绑定的错值。

### 4.5 语句与结果集

最终 API：

```cpp
namespace uniorm {

struct column_info {
    std::string name;
    sql_type type;                 // backend 中立枚举（types.hpp）
    std::size_t display_size;      // DECIMAL/NUMERIC 即 precision
    bool nullable;
    std::size_t scale = 0;         // 小数位数（取自 DecimalDigits）；追加在
                                   // 末尾，旧头文件读到的字段偏移不变
};

// 行式绑定结果集；move-only，pimpl；由 connection::execute 创建
class result_set {
    bool next();                   // 下一行；false = 结果耗尽
    row current();                 // 物化当前行为动态 row
    std::size_t column_count() const;
    column_info const& column(std::size_t index) const;
};

// sql_value → T：精确匹配优先，整数宽度间范围检查收窄，文本按需解析成
// 算术目标（DECIMAL 即以字面量文本进进程；带小数或超范围即拒），
// 支持 std::optional<U>；带 converter<U> 的类型走最后一支，
// value_cast<decimal_t> 即由字面量解出；失败抛 type_mismatch
template <class T> T value_cast(sql_value const& v);

// 每个结果集共享一份列名表：列名 + 名字→下标索引（describe 时构建一次）
struct column_names {
    std::vector<std::string> names;
    std::unordered_map<std::string, std::size_t> index;
    explicit column_names(std::vector<std::string> column_list);
};

class row {
    row(std::shared_ptr<column_names> names,
        std::vector<sql_value> values);
    sql_value const& at(std::string_view name) const;   // O(1) 查 index；不存在抛 column_not_found
    sql_value const& at(std::size_t index) const;
    template <class T> T get(std::string_view name) const;
    template <class T> T get(std::size_t index) const;
    bool is_null(std::string_view name) const;
    bool is_null(std::size_t index) const;
    std::size_t size() const noexcept;
    std::vector<std::string> const& names() const noexcept;
};

// 有序参数容器；值经 detail::make_sql_value 归一化：
// nullptr → NULL；整数按宽度归并（超 int64 的 unsigned 抛 type_mismatch）；
// 浮点 → double；enum → 底层类型；可转 std::string 的类型 → string
class params {
    params() = default;
    template <class... Ts> explicit params(Ts&&... values);
    explicit params(std::vector<sql_value> values);
    std::size_t size() const noexcept;
    sql_value const& at(std::size_t index) const;
    std::vector<sql_value> const& values() const noexcept;
};

// 高层连接：backend 由连接串的 scheme 或 spec 的 scheme 选定，move-only
class connection {
    explicit connection(std::string_view connection_string);
    explicit connection(connection_spec const& spec);   // 见 §5.2 的四件套
    void close();
    bool is_open() const noexcept;

    // 唯一的 result_set 出口。第三个参数是本级别的块取行大小，默认 1
    // （即逐行）；0 不被接受，result_set 退回逐行取；orm 层按自己的
    // row_array_size() 传值（见 §4.7）
    result_set execute(std::string_view sql, params const& p = {},
                       std::size_t row_array_size = 1);
    std::size_t execute_update(std::string_view sql, params const& p = {});

    // 事务控制（见 §4.9）
    transaction begin();
    void set_autocommit(bool enabled);   // 关即进入手动提交模式；打开会提交挂起的工作
    void commit();
    void rollback();
    bool autocommit() const noexcept;   // set_autocommit 最后一次设进去的模式

    // 语句缓存原语：取出已 prepare 的语句 / 按 key 归还（内部由 execute 路径使用）
    std::unique_ptr<backend::statement_iface> acquire_statement(std::string const& sql);
    void release_statement(std::string const& key,
                           std::unique_ptr<backend::statement_iface> stmt);

    // 元数据
    std::string dbms_name() const;             // SQL_DBMS_NAME，方言推断输入
    backend::capabilities caps() const noexcept;

    // 标识符拼法：空＝不声明，跟着 banner 探出的那家方言走（见 §4.8）
    std::optional<dialect::identifier_case> identifier_case() const noexcept;
    void identifier_case(std::optional<dialect::identifier_case> policy);
    dialect const& sql_dialect();     // banner 探测与上面的策略合成的那一份

    schema_meta& schema() const;               // 表自省，见 5.2

    // 逃生舱：调用方点名期望的原生句柄类型（ODBC 下 T = void 的 SQLHDBC）
    template <class T> T* native_handle() noexcept;

    // 预编译语句缓存观测（见 4.5.1）
    unsigned long long statement_cache_hits() const;
    unsigned long long statement_cache_misses() const;
    std::size_t statement_cache_size() const;
    void clear_statement_cache();
};

// 注意：connection 上没有实体 CRUD、没有批量入口、也没有聚合投影 query<T>。
// 这些都在 orm 上（§4.7 注册与 CRUD、§4.8 构建器、§4.6 投影）；
// connection 只提供上面这些原语，orm::native_connection() 是把它们露出来的逃生舱。

} // namespace uniorm
```

`params` 支持 `execute(sql, params{18, "alice"})` 这类写法；用户值一律逐个经
`statement_iface::bind_parameter`（1-based 槽位）落到 `SQLBindParameter`，
不做任何字符串拼接。

绑定形态：读侧每个结果列只 `SQLBindCol` 一次，指向调用方持有的列缓冲，
按块取行（`SQL_ATTR_ROW_ARRAY_SIZE`；`orm::row_array_size()` 默认 100，
`connection::execute` 的显式参数默认 1，块大小为 0 时 `result_set` 退回逐行——
直连 `connection` 会绕过 `orm` 的 setter）；写侧批量按 `SQL_ATTR_PARAMSET_SIZE`
分批提交，两条通道并存——行式 `bind_batch_params(std::vector<params>)`
（backend 内部转置成列存）与列式 `batch_writer_iface`（调用方直接写列缓冲，
按 `caps().columnar_batch` 选用，ODBC 为 true）。逐行读写只是块大小为 1
的退化情形。

#### 4.5.1 预编译语句缓存

`connection` 内置按 SQL 文本为键的 LRU 语句缓存（`uniorm::detail::statement_cache`，
定义在私有头 `src/statement_cache.hpp`，容量固定 64、无 setter），对调用方完全透明：
`execute` / `execute_update` / 聚合投影 `orm::query<T>` / 实体查询直绑路径
（`query<T>`）全部走 checkout/check-in 模型，公开出口即
`connection::acquire_statement()` / `release_statement()`：

- 缓存**只存空闲句柄**：取出时 `statement::reset()`（仅 `SQL_CLOSE` 关游标 +
  row array / paramset size 回落 1；重绑覆盖旧绑定，故无需 `SQL_UNBIND` /
  `SQL_RESET_PARAMS`）后复用，归还时不复位——复位推迟到下一次 acquire；
  使用中的句柄不在缓存内，同一 SQL 并发执行互不干扰（各自新建）
- `execute_update` 执行完立即归还；`execute` 的 `result_set` 携带
  check-in 闭包（`weak_ptr` 引用缓存状态），析构时归还——缓存先于
  连接句柄销毁；缓存本身是 `shared_ptr`，故连接被移动后闭包仍有效，
  连接已销毁时静默丢弃句柄
- 归还时该 SQL 已有条目（并发 checkout 期间产生过新建）则丢弃；
  超容量从 LRU 尾部淘汰
- `close()` 先清空缓存再断开，保证语句句柄先于 DBC 释放

边界：DDL（DROP/CREATE）会使同表旧 prepare 失效，驱动报错后句柄即弃用，
重试自然走 miss 路径；需要立即失效可调用 `clear_statement_cache()`。
`hits()/misses()` 计数器供测试与观测。

#### 4.5.2 批量 CRUD（数组参数绑定）

入口全在 `orm` 上，只有实体形态——没有"表名 + 列名 + 行数组"的动态批量入口：

```cpp
std::size_t n = db.insert(users);    // users: std::vector<User>（insert 只有这个重载）
std::size_t u = db.update(people);   // 按主键匹配；可传 where_fields 指定键列
std::size_t r = db.remove(people);   // 同上；无键列可推断且未给 where_fields 则抛错
```

主键推断取**全部** `is_primary_key` 列（复合键只用首列会让单实体
`update`/`remove` 命中同键前缀的其它行）。`where_fields` 在生成语句前先对映射
逐个解析：未命中的名字抛 `mapping_error`（静默丢弃会让占位符比绑定参数多，
驱动报的错与真正写错的字段名毫无关系），空列表抛
`"update|remove: no WHERE fields specified"`，全部列都在 WHERE 里则没有可赋值的列，
抛 `"update: no columns to set (all mapped columns are in WHERE)"`。

实现要点：

- 语句只含**一行**占位符组：`INSERT INTO <表> (<列…>) VALUES (?, ?, …)`
  （`UPDATE` / `DELETE` 同形），行与行靠**数组参数绑定**展开——
  `SQL_ATTR_PARAMSET_SIZE` 决定每次 `SQLExecute` 提交的参数集个数，分批粒度是
  `orm::paramset_size()`（默认 1000；setter 把 0 归一为默认值，getter 因而总是
  报出实际生效值——列式分批循环以该值为步长，0 会原地打转）。`orm::row_array_size()`
  同理，两个默认值分别是 `orm::default_paramset_size` /
  `orm::default_row_array_size`。标识符一律经
  `dialect::quote_identifier`
- 两条写通道，按 `caps().columnar_batch` 选择（ODBC 为 true）；选择点在
  `src/orm.cpp` 的实体写入口，两条通道因此共用同一份语句文本与缓存键：
  - **列式**：backend 经 `batch_writer_iface` 按列分配 typed buffer，变长列 stride
    取该块内最大字节数（字符列 +1，列式路径另有 65 字节下限），实体字段经
    `column_meta::write_to_param_buffer` 直写缓冲，**只 `SQLBindParameter` 一次**，
    后续块只覆写缓冲内容 + 重设 paramset size 再 execute；
  - **行式**：`bind_batch_params(std::vector<params>)` 把行式 `sql_value` 转置成列
    数组，逐块重绑
- 顺序上先写缓冲后绑定：部分驱动（MariaDB ODBC）在 `SQLBindParameter` 时就会读缓冲内容
- `orm::auto_commit()`（默认 true）就是**连接的 autocommit 属性**，管住这个 orm 的
  所有写：开着时单实体 `update`/`remove`、`execute`/`execute_update` 与批量都是
  **调用返回即落盘**，批量额外再包一层事务以取得"整批要么全成、要么全不作废"；关掉时
  三类写一律挂起，直到调用方 `commit()`。把提交粒度放进连接而不是批量入口，正是为了让
  单行与批量对外行为一致（见 §4.9）。调用方自己 `begin()` 时批量并入外层事务，
  由外层决定提交还是回滚
- 空容器直接返回 0，不取语句；`std::optional` 空值写 indicator =
  `backend::null_indicator`（行式通道把整段 indicator 预置为 NULL，只覆写非空行）
- 行数语义两条通道不同：列式 insert 统计**提交**的行数，行式路径累加
  `affected_rows()`
- `update()` / `remove()` 返回的正是那个累加值，而数组绑定时它是**驱动**给的：
  psqlODBC `16.00` 把每组参数都执行了，`SQLRowCount` 却停在其中一组的行数，两支
  MySQL 线连接器报的是整组的总数（5 组全命中：`1` 对 `5`；一组不命中：`1` 对 `4`），
  逐组 execute 再累加在三者上都得到同一个数。于是 `caps()` 多一项
  `array_rowcount_totals`，ODBC 实现按 `SQL_DRIVER_NAME` 认出 psqlODBC 并清零它，
  核心随即把这两处累加受累行数的批量写——`update()` 与 `remove()`——的分批粒度
  收成 1，两条写通道都受这个粒度管。多付的只是客户端的 execute 次数——服务端那边
  MariaDB 的连接器本来就把 5 组压成一条命令、Connector/ODBC 本来就发 5 条，逐组与
  整组在两者上收到的命令数一样。列式 insert 数的是绑定的行数，不受此影响
- 因此 `column_meta` 除 `write`（populate）/`read`（行式值提取）/`make_binding`
  （查询直绑）外，还带列式通道要用的 `write_to_param_buffer` 与 `get_string_size`

### 4.6 聚合 struct 投影（零注册路径）

`T` 未注册时按列序绑定聚合体（注册过则由 §4.7 的映射接手）：

```cpp
struct user_row {
    int64_t id;
    std::string name;
    std::optional<int32_t> age;
};

// orm 上的模板重载（connection 没有 query 成员）；实体查询走 db.query<T>()
auto rows = db.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", {18});   // std::vector<user_row>
for (auto const& u : rows) { /* ... */ }
```

返回前按 `orm::row_array_size()` 块取行，逐行 `fill_into` 直接构造进结果 vector
的存储（见 §4.8 物化路径）。块读循环只有一份，在 `detail::fetch_rows`
（`include/uniorm/detail/fetch_rows.hpp`）：`orm::query<T>(sql)` 的两条分派与 §4.8
构建器的 `all()` 都调它，`one()` 调同处的 `detail::fetch_one`。它只要求 `binding`
提供 `set_row_array_size` / `bind` / `fill_into`（`one()` 那份额外要 `take`），
`projection` 与 `entity_binding` 正是同一组接口。

实现：自实现 `pfr_lite`（`detail/pfr.hpp`，不引入 Boost），含两个核心设施——
`field_count<T>`（万能转换类型 + `requires` 表达式探测聚合体字段数）与
`tie_aggregate(T&)`（按字段数 `if constexpr` 分派的结构化绑定展开，由宏生成至上限）。
要求：入口 concept `detail::aggregate_projection` = `std::is_aggregate_v` +
`std::is_default_constructible_v` + `field_count<T>() <= max_aggregate_fields`（64）；
字段类型满足 4.3 的映射或存在 converter；嵌套聚合体计为一个字段，需经 converter
绑定。

列序就是这条路径的契约：`projection::bind` 把第 k 个字段绑到结果第 k 列，它手上没有
字段名可查（`tie_aggregate` 的结构化绑定只给类型与顺序，`f1..f64` 是占位名），所以
重排 SELECT 会把同类型字段的值静默对调，只有类型不同时才由后端报错。字段数多于结果
列数会在绑定期被后端拒掉，少于则多出来的列被忽略。

`orm::query<T>(sql)` 先问 `T` 注册过没有（`orm::find`）：注册过就交给 §4.7 的
`entity_binding`，序号由 `entity_meta::result_ordinal` 按标签定位，列序不再参与；
没注册才走上面这条按序投影。注册与否是运行时事实，所以这个入口的模板参数不再挂
concept，体内只留 `static_assert(std::is_default_constructible_v<T>)`（两条路径都
要就地构造 `T`），既未注册又非聚合体的类型在运行时抛 `mapping_error`。

### 4.7 实体映射（显式契约）

最终 API：

```cpp
namespace uniorm {

enum class validation_mode { strict, lenient };

// 类型擦除的成员指针：typeid(所属类) + 成员指针字节表示（memcpy）
struct member_key {
    std::type_index owner;
    std::vector<std::byte> repr;
    bool operator==(member_key const&) const = default;
};
template <class T, class M> member_key make_member_key(M T::*member);

struct column_meta {
    std::string column;          // 数据库列名
    bool is_primary_key = false;
    bool nullable = false;       // 成员是 std::optional
    backend::buffer_type buffer_type;   // 由成员类型推出（member_buffer_type<M>()）
    sql_type_set accepted_types;        // 该表示可绑的 sql_type 位掩码（accepted_sql_types<M>()）
    member_key key;
    std::function<void(void*, sql_value const&)> write;  // populate 用写闭包
    std::function<sql_value(void const*)> read;          // 行式路径的值提取
    std::function<std::unique_ptr<detail::field_binding>(void*)> make_binding;  // 查询直绑
    // 列式批量写：把 obj 的该字段写进 column 缓冲第 row 行（buffer + row*stride）
    // 并置 indicators[row]（空 optional → null_indicator）。返回值是成员的
    // buffer_type，批量路径并不使用它——分配时用 column_meta::buffer_type
    std::function<backend::buffer_type(void const* obj, std::size_t row,
        void* buffer, std::size_t stride, std::int64_t* indicators)> write_to_param_buffer;
    std::function<std::size_t(void const* obj)> get_string_size;  // 变长列缓冲定尺；空值 0
};

struct identifier_resolution {
    schema_meta::table_ref table;
    std::vector<std::string> columns;
};

struct entity_meta {
    std::string table;
    std::vector<column_meta> columns;
    std::vector<member_key> ignored;
    std::optional<identifier_resolution> resolved;

    std::string const& column_name(member_key const& key) const;
    std::string table_sql(dialect const&) const;
    std::string column_sql(std::size_t index, dialect const&) const;
    std::string column_sql(member_key const&, dialect const&) const;
    void populate(void* obj, row const& result_row) const;
};

// 成员类型须满足 readable_member：bool/int8~64/double/string/bytes/timestamp、
// 上述类型的 std::optional，或存在 uniorm::converter 特化的域类型（编译期 static_assert）
template <class T>
class mapping_builder {                       // db.map<T>("table") 的返回值
    template <class M> mapping_builder& column(std::string_view column, M T::*member);
    template <class M> mapping_builder& primary_key(std::string_view column, M T::*member);
    template <class M> mapping_builder& ignore(M T::*member);   // transient
};                                    // 返回对象的生命周期即注册生命周期，无 end()

// 高层入口：持有 pooled_connection（不是裸 connection，也不直接持有池）
class orm {                             // 非线程安全，按线程/会话持有
    orm();
    explicit orm(std::string_view connection_string);   // 经 connection_pool_registry 取池
    explicit orm(connection_spec const& spec);          // 也走注册表（§4.10）
    explicit orm(connection_pool& pool);                // 用户自管池
    void connect(std::string_view connection_string);
    void connect(connection_spec const& spec);
    void disconnect();

    // 同名 getter/setter，默认值见括号
    std::size_t row_array_size() const noexcept;  void row_array_size(std::size_t);  // default_row_array_size = 100
    std::size_t paramset_size() const noexcept;   void paramset_size(std::size_t);   // default_paramset_size = 1000
    bool auto_commit() const noexcept;            void auto_commit(bool);            // true：即连接的 autocommit 属性（见 §4.9）
    std::optional<dialect::identifier_case> identifier_case() const noexcept;
    // 标识符拼法：空＝不声明，跟着连接的方言走；租到连接时重贴（见 §4.8）
    void identifier_case(std::optional<dialect::identifier_case>);

    template <class T> mapping_builder<T> map(std::string_view table);  // 重复注册抛 mapping_error
    template <class T> entity_meta const& meta() const;                 // 未注册抛 mapping_error
    entity_meta const* find(std::type_index type) const;
    std::size_t size() const noexcept;

    void resolve_identifiers(std::string_view catalog, std::string_view schema);
    void clear_identifier_resolution() noexcept;
    void validate(validation_mode mode = validation_mode::strict);
    // 未解析映射按大小写策略查旧目录；已解析映射精确重读选中的身份（§4.8）。
    // 校验不修改映射，逐实体核对：
    //  - 表不存在（形状为空；或形状虽有，清单里却没有这一精确拼法、另有一个只差
    //    大小写的——名字比对不分大小写的服务端就是这样，列是顺带约来的，错仍在表名）
    //    → mapping_error
    //  - 列缺失                       → mapping_error
    //    （清单里这一拼法本就独有，或压根没有只差大小写的另一种时才到这里：目录不含
    //    视图，而视图的列读得到，那一列就是真缺的）
    //  - 列可空但成员非 optional      → strict 抛 mapping_error / lenient 放行
    //  - 列类型族与成员的 accepted_types 不符 → strict 抛 mapping_error（§4.3）；
    //    converter 成员的族由其表示决定；驱动归类为 sql_type::other 的列跳过

    std::size_t insert(std::vector<Entity> const& rows);                 // 仅批量
    std::size_t update(Entity const&);                                   // 主键推断：全部键列
    std::size_t update(Entity const&, std::vector<std::string> const& where_fields);
    std::size_t update(std::vector<Entity> const&, /*可选 where_fields*/);
    std::size_t remove(Entity const&);                                   // 同上三形
    std::size_t remove(std::vector<Entity> const&, /*可选 where_fields*/);
    // where_fields 先对映射解析：未命中抛 mapping_error，空列表与
    // "全部列都进了 WHERE" 抛 uniorm_error（见 4.5.2）
    template <class T>
    ::uniorm::query<T> query();                               // 实体查询：db.query<T>()
    template <class T> ::uniorm::update<T> update();          // 类型化 UPDATE builder
    template <class T> ::uniorm::remove<T> remove();          // 类型化 DELETE builder
    template <class T>
    std::vector<T> query(std::string_view sql, params const& bound = {});
    // 类型化读：T 注册过按 §4.7 的列名配对，否则按 §4.6 的列序投影
    update_builder update(std::string_view table);          // 无映射表的动态 UPDATE
    remove_builder remove(std::string_view table);          // 动态 DELETE

    result_set execute(std::string_view sql, params const& p = {});      // 转发 row_array_size()
    std::size_t execute_update(std::string_view sql, params const& p = {});
    transaction begin();  void commit();  void rollback();

    unsigned long long statement_cache_hits() const;    // 转发底层 connection
    unsigned long long statement_cache_misses() const;
    std::size_t statement_cache_size() const;
    void clear_statement_cache();
    schema_meta& schema();                          // 表自省，见 5.2
    connection& native_connection();                     // 逃生舱（未连接即抛）
};

} // namespace uniorm
```

单个实体版 `update` / `remove` 用 `requires(!std::is_convertible_v<Entity const&,
std::string_view>)` 与动态表名版区分；无主键又没给 `where_fields` 时抛
`uniorm_error`。键列推断与 `where_fields` 解析集中在私有头 `src/orm_mapping.hpp`
的 `detail` 助手（`primary_key_columns` / `resolve_where_columns` /
`resolve_where` / `set_columns_of`）里，四个实体写入口共用同一套规则，规则本身见
4.5.2。实体容器经这些模板擦除类型后交给 `src/orm.cpp`——模板只负责传下
`meta<Entity>()` 与 `(data, sizeof(Entity), size)`，余下的 WHERE 解析、SET/WHERE
列划分、语句文本、通道选择与事务都编译在库里，`Entity` 本身只在头文件这一层出现。
同一条纪律也管住了非模板成员：配置访问器（`row_array_size` / `paramset_size` /
`auto_commit` / `identifier_case`）、`find` / `size` 与 `native_connection` 都定义
在 `src/orm.cpp`，头文件里只剩下声明、两个 `default_*` 常量，以及必须由调用方实例
化的模板。写路径
因此并未变慢：批量入口读的是 `paramset_size_` 成员本身，是否自开事务看
`connection::autocommit()`（一个缓存的提交模式），每行一次的循环里没有新增
任何跨库调用。

成员指针的类型擦除：注册时经 `make_column_meta` 捕获 `write` 闭包
（`[member](void* obj, sql_value const& v) { static_cast<T*>(obj)->*member = value_cast<M>(v); }`），
运行时物化不再需要模板。注册实体读回的结果按**列名**（而非列序）配对——构建器自写的
SELECT 与 `orm::query<T>(sql)` 的手写语句都走这套，列序与字段顺序不一致不再影响结果；
按序号绑定的只剩零注册的聚合投影，那是 §4.6 的列序契约。

### 4.8 成员指针查询构建器

```cpp
auto users = db.query<User>()
    .where(gt(&User::age, 18) && eq(&User::status, Status::Active))
    .order_by(&User::name, direction::desc)
    .limit(50)
    .all();                        // std::vector<User>

auto count = db.query<User>()
    .where(col(&User::age) > 18)
    .count();

auto opt = db.query<User>()
    .where(col(&User::id) == 42)
    .one();                        // std::optional<User>
```

谓词写法说明：不使用 `&User::id == 42` 这类直接对成员指针重载运算符的形式。
原因有二：C++20 下模板化 `operator==` 会与"翻转候选"重写规则冲突产生歧义；
且 GCC 11 对成员指针操作数不做 ADL，运算符在多数编译器上根本找不到。
因此提供命名函数 `eq/ne/lt/le/gt/ge`，以及 `col(&User::x)` 包装后的中缀写法；
`&&` / `||` / `in` / `is_null` / `like` 作用于 `predicate` 本身，不受影响。

入口在 `orm` 上而非 `connection` 上（`connection` 没有 `query` 成员）：
`db.query<T>()` 直接产出 `query<T>` 构建器，构建器只持一个 `orm*`；连接经
`orm::native_connection()` 取得，事务语义因此跟随 `orm` 当前持有的连接
（`orm` 内部持 `std::optional<pooled_connection>`，见 §4.10）。构建器可以在
语句之间存下来，因为它不再依赖一个中间网关的生命周期。

链式部分与渲染集中在 `query_shape<T, Self>`（同文件）：`where` / `order_by` /
`limit` / `offset` 返回 `Self&`（CRTP，链上仍是派生构建器本身），`render_select`
与 `render_count` 交出 `statement{ sql, bound }`——文本与占位符要绑的值成对给出，
`build_select()` 只是丢掉 `bound` 取文本。`query<T>` 继承它，只声明 `all` / `one` /
`count` 三个终端，终端体内是基类的 `run_all` / `run_one` / `run_count`。

物化路径：`all()`/`one()` 不经过 `result_set`/`row`/`sql_value`，而是把结果列
**一次 `bind_column` 到暂存缓冲（ODBC 下即一次 `SQLBindCol`），再按字段偏移直写
目标实体**。注册时 `column_meta` 除 `write`/`read` 闭包外再生成 `make_binding`
工厂（`detail::make_field_binding` 按成员类型选择数值直绑、字符串/二进制定长
缓冲 + 截断重读、时间戳暂存、`std::optional` 空值复位等绑定策略）。变长列的
暂存槽宽取 `display_size`：字符列 `display_size × 4 + 1`（`display_size < 1024`
时），二进制列 `display_size + 1`（`< 4096` 时），否则退回 4096 定宽——使一个块
的缓存占用与列宽成比例；超槽或驱动回报 `SQL_NO_TOTAL` 的值退回
`read_long_text`/`read_long_bytes` 整读。这些 `display_size` 来自一条查询**只
描述一次**的 `stmt.column_meta()`：`projection::bind` 与 `entity_binding::bind`
各取一轮结果元数据，再连同序号传给每个绑定——让绑定自己描述，C 个变长列就要付
C 轮 `SQLDescribeCol`。每个绑定在构造期捕获字段在实体内的**字节偏移**，
`finalize(row_index, entity)` 把某行的暂存数据直接写入任意实体实例。`all()`
因此可以按块 `resize` 出 vector 存储后逐行 `fill_into(out[i])`（异常时把 size
截回原值再重抛），无原型对象中转、每字段一次赋值即完成物化；`one()` 仍经
`take()` + `proto_` 返回。

映射好的物化按**列名**定位序号：`entity_binding::bind` 不把注册序当序号，而是
拿 `meta.columns` 每列的标签在结果集描述里找（`entity_meta::result_ordinal`：
先精确同名，再退一次 ASCII 折叠同名，取第一个命中）。退折叠那次是因为服务端
回报的拼法可能和声明的只差大小写——引号里的声明名照样选中该列，回报的列名却是
另一拼法。结果里没有这个标签就抛 `column_not_found`，多出来的列忽略。定位只在
绑定期做一次，每行仍按序号写回，块读与按序绑定一样快。`render_select` 照注册序
列出 SELECT 列，所以构建器自写的 SELECT 每次都落在精确同名上；这套匹配要防的是
手写 SQL、`SELECT *` 和换库后列序不同的语句。

`all()` 的完整执行序列（`query<T>` 直接操作 `statement_iface`，没有中间层）：
`acquire_statement(sql)` → `bind_params` → `execute` → `set_row_array_size` →
`entity_binding::bind` → 循环 `fetch()` / `rows_fetched()` 物化 →
`release_statement`。结果向量在 execute 后按 `result_row_estimate()`
（ODBC 下为缓冲结果集的 `SQLRowCount`，不可用时为 0）预分配容量。
相比经 `row` 物化，每行省去 `sql_value` 构造与按名查找，数值列零拷贝、
变长列少一次中转拷贝；`count()` 仍走 `connection::execute` 的 `result_set`
单值路径。

表达式模板（`expression.hpp`）最终 API：

```cpp
// 谓词树节点；叶节点存 member_key + 绑定值，列名在生成 SQL 时经 resolver 解析
class predicate {
public:
    using resolver = std::function<std::string(member_key const&)>;

    static predicate comparison(member_key, std::string_view op, sql_value);
    static predicate in_list(member_key, std::vector<sql_value>);   // 空列表 → "1 = 0"
    static predicate null_check(member_key, bool negated);
    static predicate like_expr(member_key, std::string pattern);
    static predicate conjunction(predicate lhs, predicate rhs);
    static predicate disjunction(predicate lhs, predicate rhs);

    // 渲染为 '?' 占位符 SQL，绑定值按序追加进 out
    std::string to_sql(resolver const& resolve, std::vector<sql_value>& out) const;
};

// 谓词构造器（值经 detail::make_sql_value 归一化）
template <class T, class M, class V> predicate eq/ne/lt/le/gt/ge(M T::*member, V&& value);
template <class T, class M, class V> predicate in(M T::*member, std::initializer_list<V> values);
template <class T, class M, class V> predicate in(M T::*member, std::vector<V> const& values);
template <class T, class M> predicate is_null(M T::*member);
template <class T, class M> predicate is_not_null(M T::*member);
template <class T, class M> predicate like(M T::*member, std::string_view pattern);
predicate operator&&(predicate lhs, predicate rhs);
predicate operator||(predicate lhs, predicate rhs);

// 中缀风格：col(&User::age) > 18（column_ref 为类类型，ADL 可达）
template <class T, class M> column_ref<T, M> col(M T::*member);
```

- 列名渲染：成员指针 → `entity_meta::column_sql`（未注册成员抛 `mapping_error`）；
  `column_name` 保留声明词汇，SQL 选择声明名或单独保存的解析结果；
- 生成 SQL 使用 `?` 占位符（ODBC 原生参数标记），值按序收集进 `params`；
- 标识符引用与分页语法经 `dialect` 生成：

```cpp
struct dialect {
    enum class identifier_case { keep, lower, upper };
    enum class qualification { unsupported, schema, catalog };

    char quote_open = '"', quote_close = '"';     // MySQL/MariaDB → ` `
    bool ansi_pagination = true;                  // false → LIMIT/OFFSET
    identifier_case identifiers = identifier_case::keep;
    qualification table_qualification = qualification::unsupported;

    std::string fold_identifier(std::string_view identifier) const;
    std::string quote_identifier(std::string_view identifier) const;
    std::string quote_exact_identifier(std::string_view identifier) const;
    // ANSI: " OFFSET n ROWS FETCH NEXT m ROWS ONLY"；否则 " LIMIT m OFFSET n"
    std::string pagination(std::optional<std::size_t> limit, std::size_t offset) const;

    static dialect detect(std::string_view dbms_name);   // 输入 connection::dbms_name()
};
```

折叠只按 ASCII，不走 locale：同一个名字因进程 locale 不同而指向两张表，比大小写
本身更糟。策略可不声明：不声明就按 banner 探出的那家方言折——Oracle 折 `upper`，
其余家答 `keep`，所以不认识这套的人行为一字不变；声明了以声明为准，`keep` 也是
一次声明。策略住在 `connection`（banner 与它是同一处的两个输入），
`sql_dialect()` 把探测与策略合成一份方言、首次用到才建；未解析映射的 SQL 和校验
按该策略折叠。`orm` 在租到连接时重贴策略，与 `auto_commit` 相同：没有声明就贴回
没有声明，前一次租约的声明不跟着连接回来。不从连接串解析额外配置。

**显式逐名解析（0.3）**：注册后调用
`resolve_identifiers(catalog, schema)`，只处理指定命名空间里的普通表。
PostgreSQL 要求当前数据库及非空 schema，MySQL/MariaDB 要求非空 database 及空
schema；其他 DBMS 报不支持。先精确匹配表名，再独立匹配每个列名；精确不中时只接受
唯一的 ASCII 大小写候选，缺失或歧义抛 `mapping_error`。同一实体的多个声明列不能
指向同一个实际列；不同实体可以映射同表。命名空间不折叠，也不猜 search_path。

`entity_meta::resolved` 是可选的 `identifier_resolution`，保存完整 `table_ref`
和按注册顺序排列的实际列名，原有 table/columns 声明不变。所有结果先暂存，全部成功
才以不抛异常的交换安装；失败保留旧结果，且不替换实体对象或 columns 容器。
重复解析始终从声明出发，避免 `Users → users` 后换库误选 `users`。

所有映射 SQL 通过 `table_sql` / `column_sql` 输出：解析后直接引用实际名称，
不再折叠；未解析时按旧策略输出。限定名逐段引用，闭合引号加倍，NUL 名称拒绝。
PostgreSQL 输出 schema.table，MySQL 系输出 database.table。显式 WHERE 字段和
`column_name()` 仍使用声明名；实体物化仍按序号，`populate()` 则使用实际列标签。
SQL 字符串是缓存键，因此名称变化不要求额外清空 statement cache。

`validate()` 不解析、不修改映射，已解析时通过精确元数据接口重读选中对象，沿用
类型族/可空性及 strict/lenient 规则。`clear_identifier_resolution()` 清除结果；
重连开始前及断开时也清除，重连失败不恢复旧结果。移动时记录随映射和连接转移。
已解析实体不能再通过 builder 修改；先清除才能扩展，新实体可注册为未解析状态。
解析、清除、注册不能与查询/结果使用并发，外部 DDL 变化后由应用重新解析或校验。

构建器与网关最终 API：

```cpp
enum class direction { asc, desc };

template <class T>
class query {                                  // 由 orm::query<T>() 产出
public:
    query& where(predicate p);                       // 多个 where 以 AND 连接
    template <class M> query& order_by(M T::*member, direction dir = direction::asc);
    query& limit(std::size_t n);
    query& offset(std::size_t n);
    std::string build_select() const;                // 干跑生成 SQL，不执行
    std::vector<T> all();
    std::optional<T> one();                          // 渲染时强制 limit 1
    std::int64_t count();                            // SELECT COUNT(*)，走 result_set
};

template <class T>
class update {                                 // 由 orm::update<T>() 产出
public:
    template <class M, class V> update& set(M T::*member, V&& value);  // nullptr 写 NULL
    update& where(predicate p);                      // 累加，AND 连接
    std::size_t execute();                           // 无 set 或无 where 抛 uniorm_error
};

template <class T>
class remove {                                 // 由 orm::remove<T>() 产出
public:
    remove& where(predicate p);
    std::size_t execute();                           // 无 where 抛
};

// 无实体映射的表上的动态 UPDATE / DELETE：orm::update(table) / orm::remove(table)
class update_builder {
public:
    template <class V> update_builder& set(std::string_view column, V&& value);
    update_builder& where(std::string_view clause, params p = {});  // 空白 clause 即抛；多次调用累加
    std::size_t execute();                           // set 空或未调用过 where 即抛，不放全表火
};

class remove_builder {
public:
    remove_builder& where(std::string_view clause, params p = {});  // 同上
    std::size_t execute();                           // 未调用过 where 即抛
};
```

类型化 `update<T>` / `remove<T>` 与动态 `update_builder` / `remove_builder` 都
以 `.execute()` 收尾：前者用成员指针指定 SET 列和 WHERE 谓词，后者按列名和
`?`-参数对无映射的表下手写片段。两者是各自独立的两套 `where`。

`update_builder` / `remove_builder` 的 `where(clause, p)` 与 `query<T>::where(predicate)`
是两套东西：前者把 `clause` **原样拼进 SQL**（表名与列名经 `dialect::quote_identifier`，
WHERE 片段本身不解析也不引用，值仍以 `?` 绑定，参数排在 SET 值之后）。
多次 `where()` 按调用顺序 `AND` 起来，绑定参数依次排在后面，语义与 `query<T>::where` 累加对齐。

v1 支持的谓词：`= != < <= > >=`、`&&`、`||`、`in(...)`、`is_null` / `is_not_null`、`like`；不支持子查询、join（join 场景引导用户走原生 SQL + 投影）。

### 4.9 事务

最终 API：

```cpp
class transaction {                    // move-only，RAII
public:
    explicit transaction(connection& conn);   // 仅在 autocommit 还开着时才关掉它
    ~transaction();                    // 仍 active 则 rollback（析构不抛异常）

    void commit();                     // 提交；自己关掉的 autocommit 才恢复
    void rollback();                   // 回滚；自己关掉的 autocommit 才恢复
    bool active() const noexcept;
};

transaction connection::begin();       // 等价于 transaction(conn)
transaction orm::begin();              // ensure_connected() 后转发到内部连接
bool connection::autocommit() const noexcept;   // set_autocommit 最后一次设进去的模式
```

`orm` 另有 `commit()` / `rollback()` 直通底层连接（不产生 `transaction` 对象，
即不接管 autocommit 的恢复）；`orm::auto_commit(false)` 关的就是连接的
autocommit 属性，所以此后该连接上的**每一条**写——单实体 `update`/`remove`、
`execute`/`execute_update`、批量——都挂起到调用方 `commit()` 为止。切换这个属性
本身就是一次连接属性设置：关掉只是进入手动提交模式，事务要等驱动在下一条语句处
开启；打开则把挂起的工作交给驱动提交（ODBC 对该属性的规定，MariaDB ODBC 驱动实测如此）。

批量写入口只看 `connection::autocommit()`：**连接已在手动提交模式就不自行
begin/commit**，整批并入外层，由外层决定提交还是回滚；否则整批包进一个事务，
结束提交。这一条不是可选项——`transaction::commit()` 落到的是连接级的
`SQLTransact`，一次批量结束会把调用方尚未写完的事务一并提交掉。同理，
`transaction` 只恢复**自己**改动过的提交模式：外层已经手动提交时，内层结束时
不把 autocommit 强开回来，否则调用方之后的写会悄悄脱离事务。
`autocommit()` 读的是 `connection` 缓存的提交模式（`set_autocommit` 是唯一写点），
它比"有没有活着的事务"宽：绕过 `transaction` 手写 `set_autocommit(false)` 后一条语句
都没跑，也算手动提交模式。这些调用点问的本来就是"我要不要改动模式"，宽出的一边正是
所需；池的复位最坏只多付一次空 `rollback()`。代价是该缓存需要准确：`rollback()`
自身抛异常时 autocommit 恢复不到 true，此后批量写会持续并入而不提交——比误提交安全，
方向上是对的。

v1 不支持嵌套事务/savepoint。`transaction` 只在自身析构时回滚未提交的工作。
归还的连接由 `connection_pool::release` 清理：`autocommit()` 为假（停在手动提交模式）
就先 `rollback()` 再 `set_autocommit(true)`——挂起的那笔工作属于一场已经结束、没有人再
驱动的租约，直接传给下一个借用者会变成它的，而先 `set_autocommit(true)` 又会把它
**提交**掉（驱动的语义），所以顺序不能反。复位失败（连接已死，`rollback()` 抛）的
连接被淘汰而不是留在池里，同时扣回名额，池的总数不会因此虚高。两个调用都是网络
往返，故都在池锁之外做。

`orm` 一侧的 `adopt_connection()` 保留：它还得把模式调成这个 `orm` 自己要的
`auto_commit()`，而池只会复位到默认的自动提交。清理发生在归还这一刻，所以调用方对
自己手里的连接 `auto_commit(true)` 仍然是提交挂起的工作，不是丢弃。

这里有一条对驱动的假设，两族驱动的实测不同：ODBC 规定新连接的 `SQL_ATTR_AUTOCOMMIT`
默认即 ON，MariaDB Connector/ODBC 照做（连 `@@global.autocommit=0` 的服务端也会被它
覆写成 ON），MySQL Connector/ODBC 则让会话继承服务端的 `@@global.autocommit`，且因为
它内部本就记着 ON，`adopt_connection()` 那次 `set_autocommit(true)` 是个空操作——服务
端以 `--autocommit=0` 起时，uniorm 以为的每一条自提交，实际都堆进一笔永不结束的事务：
别的连接永远看不到提交，DDL 还会被它钉住的元数据锁一直卡住。库不为此偷发
`SET autocommit=1`（那是服务端专有 SQL），把它当作调用方须满足的前提：连接串的目的地
若是一台自动提交关掉的服务端，请自行保持两侧一致。

### 4.10 连接池（最小版）

最终 API：

```cpp
struct pool_options {
    // 这个池反复打开的就是它：调用方自己的连接串，或交给 backend 去拼的四件套
    std::variant<std::string, connection_spec> target = std::string();
    std::size_t size = 8;
    std::chrono::milliseconds acquire_timeout{5000};
    std::chrono::milliseconds heartbeat_interval{30000};  // 0 关闭后台维护线程
    std::chrono::milliseconds max_idle_time{600000};
    std::string heartbeat_sql = "SELECT 1";
};

class connection_pool {                // 内部互斥锁 + 条件变量；不可移动/拷贝
public:
    explicit connection_pool(pool_options);   // 使用 shared_environment()
    pooled_connection acquire();       // 懒创建；超时抛 pool_timeout；
                                       // 建连失败时归还名额并原样重抛；
                                       // 空闲超过 max_idle_time 的连接直接丢弃
    std::size_t capacity() const;
    std::size_t idle_count() const;
    unsigned long long heartbeats_executed() const;
};

class pooled_connection {              // move-only；析构时归还池
public:
    explicit operator bool() const noexcept;   // 是否仍持有有效连接
    bool is_open() const noexcept;             // 且底层 connection 仍打开

private:
    friend class connection_pool;
    friend class connection_pool_registry;
    friend class orm;
    connection& get() noexcept;        // 取用一律经 orm / 池的公共入口
    connection* operator->() noexcept;
};

// 全局池注册表：orm 用连接串或四件套构造时即从这里借连接（自动池化）
class connection_pool_registry {
public:
    static connection_pool_registry& instance();
    pooled_connection acquire(std::string const& connection_string);
    pooled_connection acquire(connection_spec const& spec);
    void configure(std::string const& connection_string, pool_options opts);
    void configure(connection_spec const& spec, pool_options opts);
};
```

`orm` 的连接路径：`orm(connection_string)` / `orm::connect(...)` 走注册表
（key 为解析出的 `dsn=` + `uid=`（含 `user=`/`username=` 别名），大小写与空白
归一后取 `dsn=<值>|uid=<值>`；首次见到的那串完整连接字符串成为该池的建连串），
缺省参数即上面的 `pool_options` 默认值；`orm(connection_spec)` /
`orm::connect(spec)` 也走注册表，key 取 `spec=<scheme>|target=<值>|user=<值>`，
password 与字符串路线一样不入 key（同一个地方、同一个用户共用一池）。**两种写法
即使指向同一个库也不共用 key**：判定 `DSN=mydb` 与
`{scheme: odbc, target: mydb}` 是不是一处，要的是 driver 的词汇，核心没有。
`orm(connection_pool&)` 用调用方管的池；`orm()` + `map()` 只做注册，
首次使用时 `ensure_connected()` 若仍未连接
就抛。`configure()` 是 `emplace` 语义：**同 key 已存在池时静默无效**，要改
参数得在建连前调用。

全局调度线程（所有池共享一个，首个启用维护的池创建时启动，注册表清空后退出；
`heartbeat_interval > 0` 的池注册为 `weak_ptr`，进程内不随池数量增长线程）：

- 每个周期先把**空闲列表**里超时（`now - released_at > max_idle_time`）的条目
  摘下并归还名额（`created--`），随后在**锁外**销毁以断开
  （`SQLDisconnect` 可能阻塞，不能持锁执行）
- 其余空闲连接整体移出 `idle` 并在锁外逐个执行 `heartbeat_sql`（经
  `connection::execute`，因而走语句缓存）；执行失败视为连接已死，直接丢弃；
  成功则带着原 `released_at` 放回（空闲时长跨心跳累计）。被移出的条目计入
  `under_maintenance`，`idle_count()` 报 `idle + under_maintenance`，故一次心跳不会
  让读数短暂掉零；条目要等自己那次往返证明活着才回到 `idle`，在此之前取不走
- 判定**逐条发布**：`--under_maintenance` 与放回 `idle` 在同一次临界区里完成，总数
  不留空隙，所以已死连接只被多算它自己那次往返，先判活的能立刻被中途到达的
  `acquire()` 借走。攒到 pass 收尾再发布的话，池满时借用方要干等
  `acquire_timeout`（实测最坏 1000 ms；逐条发布后 0.88–1.19 ms）
- 每条判定后 notify 一次，无论活死：回到 `idle` 的幸存者和归还的名额一样，都是
  某个等待者在等的东西
- 丢弃会归还名额（`created--`），等待者可触发懒创建
- 心跳执行期间不持有池锁，`acquire/release/idle_count` 不被阻塞；逐条发布等于
  pass 里多 N 次加解锁，换来的是上面那笔等待
- 调度循环按各池 `next_tick` 最早截止时间等待；池析构**不**反向注销自己——注册表
  持有的池比调度器那个函数内静态活得久，去碰它就是 use-after-free（ASAN 实测）——
  改由 worker 在下一趟 pass 开头清掉失效的 `weak_ptr`，因此一个死池的记录最多留一个
  心跳周期，维护过程始终不会触及已销毁的池

v1 不做：动态伸缩。池须比所有借出的连接长寿。

### 4.11 异步表面（`async.hpp`）

第一版是**同步契约之上的仿真**：一个 task 的函数体就是它包着的这次同步调用
（`orm::execute_update`、`detail::fetch_rows` 那个循环、整段批量扫描），跑在核心
worker 池的一条线程上。异步只回答三件事——这次同步调用跑在哪条线程、结果怎么交
回去、这条连接上谁排队谁在飞。没有第二条数据库通路，发出去的 SQL 一个字节不变，
`SQL_ATTR_ASYNC_ENABLE` 也没设（§4.1 那四条实测就是这一层的存在理由）。形状按
"以后某家 backend 用原生异步替掉这一层、调用方一行不改"切，那一层现在不建。

公开侧六样（`include/uniorm/async.hpp`，855 行；不进总头文件 `uniorm.hpp`，协程
因而不漏进每个 include 它的编译单元）。头里只剩声明与模板：非模板的实现全在
`src/async/` 那两个 .cpp 里。`async.cpp` 一个编译单元装下三样——会话与句柄的
构造、旋钮和那几个 task 形操作，那条 FIFO，以及接口的虚析构（放库里是让
调用方自己派生 executor 时靠链接取得这张 vtable，而不是在他那个编译单元里
另发一份）；`worker_pool.cpp` 是默认池，单开一个文件因为它要 include 私有头
`worker_pool.hpp`。头里的模板要推队列，所以 `detail::operation_queue` 导出了：
它看着像内部件，链接上却是公开表面的一部分。下面不带文件名的行号都在
`async.hpp`，函数体写作 `async.cpp:NNN`。

- `executor`（`:36`）：一个虚函数 `submit(std::function<void()>)`，交出活就返回，
  不在这里跑任何东西。必须公开，因为"跑"与"回"两个方向都可能要点名调用方自己的
  循环。
- `default_executor()` / `default_executor_size()`（`:45`、`:50`；实现
  `src/async/worker_pool.cpp:109`）：核心那条池，第一次用到时才立起来，尺寸按此前
  `default_executor_size()` 说过的那次定，此后只增不减——已经进了驱动调用的 worker
  拔不出来。**worker 条数即并发上限**，队列比它深只是有人在等。要零条在池还没立
  起来时也拒（`src/async/worker_pool.cpp:122`），拒完那个数一个字没改。worker 跑
  的是每个操作的一段，`run()`（`src/async/worker_pool.cpp:86`）把闭包的抛出吞在
  这一层：让它穿过线程会带走
  整条池，于是连带每一把等它的会话。
- `task<T>`（`:394`）：惰性，造出来时什么都不跑，被 drive 那一刻才 submit。三种
  drive 动作：`co_await`、`block()`（停住本线程等结果）、`start(handler)`（交出去
  继续走）。没被 drive 的 task 析构不留副作用；同一个 task 第二次 drive 抛
  `uniorm_error`。堆上那块共享状态是 `detail::operation_core<T>`（`:135`），由
  `shared_ptr` 计数，结果与异常都住在它里面，协程帧挂在它上面、由它的析构销毁——
  于是等结果那条线程与最后拆帧那条线程碰不上。帧从不自毁。`start()` 的 handler
  抛出没人接：它是失败的最后一段，两处跑它的地方都不许抛，于是吞在
  `deliver_failure()`（`:230`）这一层——吞的是 handler 自己，不是操作的失败。
  下面 `orm` 那一条上的回调形不在这三个动作外加第四个门：它自己不交出 task，
  所以"惰性"与"只能 drive 一次"这两条规矩对它没有可犯之处。
- `orm`（`:549`）：会话。按值揣一把 `uniorm::orm`，**不新增状态宿主**；入口与同步
  对象同款——连接串（`:555`）、四件套（`:556`）、`connection_pool` 借还（`:557`）
  三条各自直接立起会话，`uniorm::orm` 不必出现在调用方眼里：`map()` 在两个对象上
  同名同参，一份注册配方对着手里那把写一遍就够。`:560` 那条接管已配好的对象仍留
  着，说的是"连里面的租约一起拿走"，现在没有用例走它。装 task 那一组
  （`connect`/`disconnect`/`execute_update`/实体批量三件/`query` 两式/
  `auto_commit`/`commit`/`rollback`/`begin`）、四个只改内存的配置面按名字转发
  （`map`（`:575`）、`row_array_size`、`paramset_size`、`identifier_case`，外加
  `auto_commit` 的读档——写它仍是一次操作）、`run_on`（`:601`）、`resume_on`
  （`:608`）、`submit_limit`（`:612`、`:614`）、`pending()`（`:617`），以及按值
  持有的那条 FIFO。`wait_all_operations()`（`:623`）是那道屏障的公开出口：
  会话还开着的时候等一次"交出去的都落地"，长驻会话关闭又不想此刻断连接才用得上。
  目录读不转发，由 `sync()`（`:598`）交回内层对象（`validate` / `schema` /
  `resolve_identifiers` / `clear_identifier_resolution` /
  `native_connection`）。转发的代价记在这：同步侧每加一个只改内存的 setter，就欠
  下一处可能悄悄漏掉的同步，用例把四个旋钮钉在同一个宿主上（§8）。
  四个批量与读的名字各还有一层回调形重载（`:643` 起，`insert`/`update`/`remove`
  各一个、`query<T>` 两个），出口是 `std::function<void(R,
  std::exception_ptr)>`，**先看第二个参数**：失败时第一个是 `R{}`。空 `on_done`
  在调用点就被拒（`:742` 的 `answer`）：它会在 body 里抛出自家的
  `bad_function_call`，那格回合被答掉却没人听，屏障等的是永远不会发生的一次。这
  一层是第二条路，只覆盖"调一句、响一次"，builder 的三个终端与 `connect`/`begin`
  都没有它。它与 task 形把**同一个工作函数**交给同一条 FIFO（`:716` 起那四个
  `*_work`），所以"不改发出去的 SQL 一个字节"这一条对两面同时由构造成立；回调
  在 body 里响（`:531` 的 `answered`），因此它握着这一格直到响完，慢回调会挡住
  同一会话的下一条请求，而 `~orm()` 那次等待因此已经覆盖"所有回调都响过"，不需
  要第二枚计数。溢出的规矩与 task 形一样：`submit_limit` 满了从调用点抛
  `submit_overflow`，那次不会有回调（请求根本没进队列）。`on_done` 自己抛出没人
  接：它已经是这一格答案的最后一段，抛出就止在这里，会话照样接下一条。
  `resume_on` 对它没有话语权——它没有续体可交；`run_on` 仍有，那台 executor 没人
  泵，`~orm()` 就等不到这一格交回来。
- `query<T>`（`:787`）：三个终端 `all`/`one`/`count` 返回 task；装饰与渲染仍是不
  同步那一侧的 `query_shape`（§4.8），交出去的是副本，渲染发生在真正读的那条线程
  上，因为拼标识符要问连接的方言。这一层没有回调形终端。
- `transaction`（`:825`）：`begin()` 交出的句柄。`commit()`/`rollback()` 各是
  本会话的一个操作，因此排在它要收尾的那些写之后；句柄把内层
  `uniorm::transaction` 揣在 `shared_ptr` 里，于是提交闭包不依赖句柄还活着。
  析构是兜底：先等会话没有在飞的操作，再在当前线程上回滚一次，**允许阻塞**，
  且不许抛。**句柄可以活过会话**：内层那把 `uniorm::transaction` 握的是会话里
  那条连接的裸指针，而连接随 `uniorm::orm` 一起走，于是 `~orm()`
  （`src/async/async.cpp:161`）在连接还活着时先把没闭合的句柄逐个回滚；此后
  `commit()`/`rollback()` 报的是"背后的会话没了"，不是踩到悬空指针。这本账记在
  会话发出处那把 `detail::session_door`（`:503`）与一份弱句柄清单上，句柄自己
  闭不上它。

这六样在两条路上各自怎么搭手——`co_await`、`block()`、`start()` 与回调形各在哪
一刻进队列、结果从哪一条线程交付、帧由谁拆——两张时序图记在
`docs/async-sequence.md`。

每会话一条 FIFO（`detail::operation_queue`，`:85`；函数体
`async.cpp:21-90`）：同一把 `orm` 上同时只有一个操作在飞，`co_await`、
`start()` 与 `block()` 共用这一条先后，回调形那一次调用也走这一条先后——它就是
一个绑好了的操作，只占一格。排队的请求不占线程（它只是队列里一个
`std::function`），要真并行就多开几把会话。上限默认 `1024`（`:87`），满了就在
drive 动作当场抛 `submit_overflow`（`include/uniorm/error.hpp:34`——它住在错误
家族那一头，与 `pool_timeout` 同一个理由：说的是"用量不对"而不是驱动报的错）；
抛出之前没 push，**所以那个 task 没被消费**，过一会儿再 drive 是合法写法
（`async.cpp:38`）；回调形同样抛在这一处，也就是说被拒的那次请求不会有回调来。
`co_await` 是唯一一处没有调用点可抛的：那次 push 发生在 `await_suspend`
（`:365`）里，它自己接住，把它折成这一次 await 的失败，于是溢出从 body 的
`catch` 里出来、和驱动报的错走同一条路，不再穿过正在跑这一帧的那条线程。
`drain()`（`async.cpp:63`）在每次 `start()` 前后放锁，`draining_` 是内联
executor 那种重入的守卫；抛出过的 `start()` 已经答完了它那格、等不到
`finish()`，于是 `drain()` 在它的 `catch`（`async.cpp:72`）里还回回合再往下走，
否则这队列从此不再启动任何东西。启动 starter 的手只有两只：队列空着时 `push()`
当场那只（`async.cpp:42-44`，就在调用线程上），交还回合时 `finish()` 那只
（`async.cpp:50-52`，在跑完 body 最后一段的那条线程上）；出队只有一处
（`async.cpp:67`），`in_flight_` 只在 `async.cpp:68` 置真、`async.cpp:49` 与
`async.cpp:77` 置假，于是"一个 starter 恰好启动一次、一个回合只框住一个 body"
是结构而不是约定，也没有取消或丢弃的路径。`wait_idle()`（`async.cpp:58`）是屏障
本体，`~orm()`（`src/async/async.cpp:161`）与公开的
`orm::wait_all_operations()`（`src/async/async.cpp:176`）都走它——"会话必须活过它
交出去的操作"这条由库代守，不靠纪律。回调在 body 里响，所以这一句对回调面就是
"每个回调都响过"，不必再加一枚计数；同一次析构在这之后才把还开着的
`async::transaction` 句柄逐个回滚。它等的只是**已受理**的那些：被 `submit_limit`
拒掉的请求从没进过队列，屏障照样返回，那一次的回调永远不响。它等的是回合，而不是
链条：入队的只有会话操作（bind 落在 `:708` 与 `:763`），调用方自己那一帧从不
入队；一次回合又在 `finish()`（`:329`）就交出去，早于结果交付，所以一条链从
`finish()` 到下一环 `push()` 之间整条还活着、队列却是空的——这段空窗里屏障照样
返回，那一次 `block()` 可能还没醒，`start()` 的 handler 可能还没响。要在第一条
路上等到交付，自己 `block()` 或 `co_await`。

锁管的只是队列自己那一格：`push()`、`finish()`、`pending()` 与 `wait_idle()`
都在 `mutex_` 里走，所以 worker 交回合与另一条线程 drive 是串行的；bind 写进
`operation_core` 的那几项也是靠入队这把锁发布给跑它的 worker 的。会话对象本身
仍是 §4.7 那条契约——"非线程安全，按线程/会话持有"：`run_on_`、`resume_on_`
与按值揣着的注册表都没有锁，改它们不与在飞的操作并发，`sync()`（`:598`）交出的
更正是 worker 正在用的那个对象，要先落定就用屏障。`limit_` 是这一层唯一一件不等
那把锁的量：写（`async.cpp:26`）与 `push()` 在锁里的读（`async.cpp:36`）都是
relaxed atomic，于是读永远不会撕裂，但一次改写哪一刻生效不确定——和上面那两个
旋钮一样，算配置期的一次性动作。跨线程的另一处是 `detail::session_door`
（`:503`）：只有 `~orm()` 写它、句柄读它，两边可能不在一条线程上，因此它是
atomic 而不是一个 bool。

跑与回是两个方向：`run_on(executor&)` 说阻塞那一段去哪儿（默认核心池；调用方自带
池时上限归调用方算），`resume_on(executor&)` 说续体回到哪儿（默认就是跑完这一趟
的那条 worker，靠 `final_suspend` 那个 awaiter（`:323`）的对称转移把一条链保持在
一帧深；指定了才走 `hand_over()`（`:212`）那次交接）。会话定过的事优先于 drive
动作上点名的 executor。starter 可以就地启动，body 却不当场落地：默认池那次
`submit()`（`:201`）只是入队加唤醒（`src/async/worker_pool.cpp:57`），所以 body
的一段也不在调用线程上跑，没绑会话的调用方协程走的是同一处（`:190`）；
`run_on` 指一个当场就跑的 executor 时不在这条里。反过来，会话挂着手泵时对着
它 `block()` 就是吊死，`wait_all_operations()` 一样吊死——这两个形状都不写进
用例，写在注释里。

四处阻塞点是承认的，不是漏掉的：目录读留在同步侧（经 `sync()`）、
`~async::transaction` 那次兜底回滚、`~orm()` 里那次等待（它之后还要在当前线程上
回滚没闭合的句柄），以及它的公开出口 `orm::wait_all_operations()`。两处既有
陷阱同账记过：size-1 池上重连
会自锁（`orm::connect()` 在旧租约还握着时去 `acquire()`，§4.10），
异步第一版因此要求重连前先 `co_await disconnect()`；worker 上等池会饿死 worker，
FIFO 把同时想 `acquire()` 的数量限在 worker 条数以内，剩下靠 `acquire_timeout`
兜。换成可唤醒的等待者表、以及 `orm::connect()` 的旧租约顺序，是这一层后面的独立
两步，不在它里面。

ABI：一个新的安装头加新的导出类型与函数，`connection_iface`、
`statement_iface`、`capabilities`、`pool_options`、`entity_meta`、`schema_meta`
一个都不改，不移动任何 vtable 槽，`UNIORM_ABI_VERSION` 不动，仍是 0.4.x 的一格
功能加法。第二条路那五个回调形重载是这笔账里最便宜的一次加法：全是头文件里的
inline 模板，不新增导出符号，也不新增安装头。把非模板的实现挪进
`src/async/async.cpp` 之后，会话的入口、`sync()` 与那几个按名字转发的
旋钮都不再算这种便宜：它们连同显式屏障 `wait_all_operations()`、
`operation_queue` 的全部成员与 `executor` 的虚析构，是库里新添的一批导出
符号。这笔账照样不动 ABI 版本：
插件契约看的是 `connection_iface` 那一圈 vtable 槽，一处没动；`async::orm`、
`async::transaction` 与 `detail::operation_queue` 都是这一版才出现的导出类型，
没有已发布的布局可破坏，成员一个没多。换掉的只是失败的样子——新头配旧库如今
在链接期就断，不会各自带着半份实现跑起来。测试分四层
（§8）：前两层不需要任何库，第 3 层是集成用例里那一段——同一份夹具从同步与
异步两侧各读一遍、逐字段相比，一份二进制跑三条腿；第 4 层（第 2 层那批用例
换成默认 executor 再挂 `-fsanitize=thread` 重放）是本地一次性的一道，不在流水线
上：本机跑过，10 遍 0 报告，同一次构建里塞一个故意未同步的计数器它立刻报，
所以 0 是量出来的（细节见提案 §8 第 4 条）。

## 5. Backend 抽象：多后端规划与 v1 纪律

背景：libpq 与 Oracle OCI 在实际项目中都存在必须独立使用原生 API 的场景（PG：COPY、LISTEN/NOTIFY、异步 I/O；Oracle：OCI 数组绑定、高级特性等），这些是 ODBC 无法覆盖的。因此 uniorm 采用"通用层打底 + 原生特性逃生舱口"策略，而非试图把各库特性塞进统一 API。

### 5.1 边界纪律

backend 接口已在里程碑 1 落地（§5.2），下列纪律从约定变成构建结构：

- ODBC 类型（`SQLH*`、`SQLLEN`、indicator、SQLSTATE）不得出现在语句层及以上层级的
  公共 API 中：`include/` 下没有任何文件包含 `sql.h` 或写出 `SQL*` 类型，驱动类型
  只活在 `backends/odbc/`；
- SQL 方言差异集中在 `dialect`，占位符统一以 `?` 语义表达，backend 负责翻译成各自风格（libpq `$1`、OCI `:1`）；
- `transaction`、`connection_pool`、`result_set`、`orm`、`query<T>` 只依赖 `backend::*`；
- 核心单测目标 `uniorm_unit_tests` 只链 `uniorm::uniorm`，不链 ODBC：驱动类型一旦漏进
  上层公共头，这个目标就编译不过。它覆盖类型系统、pfr、`row`/`params`、
  表达式生成、映射注册、backend 注册表；高层 API（execute / result_set / 实体查询 /
  事务 / 批量）由集成测试通过真实数据库验证。
- 上一条只守得住"公共头不带驱动类型"：核心库自己的 `.cpp` 里 `#include <sql.h>`，
  在装了 `unixodbc-dev` 的机器上照样编过。真正把它逼出来的是没有那些头文件的构建，
  所以 CI 的 `core` 作业不赌镜像装没装，先拿一对读下去只会报错的 `sql.h`/`sqlext.h`
  压住 include 路径，再编（见 §8）。

**这条保证不再只到链接行为止**：每家 backend 是一个独立的共享库（当前只有
`uniorm::odbc`，产出 `libuniorm_odbc.so.<SOVERSION>`，装到
`<libdir>/uniorm/backends/`），核心 `libuniorm.so` 不链任何驱动，`ldd` 只列
`libstdc++` / `libm` / `libgcc_s` / `libc`；`libodbc.so.2` 的 `DT_NEEDED` 落在
`libuniorm_odbc.so` 自己身上。消费者的链接行始终只有 `uniorm::uniorm`，用哪家
backend 在运行时决定：`registry::create` 按 scheme 未命中时，`src/backend/plugin_loader.cpp`
`dlopen(<plugin_dir>/libuniorm_<scheme>.so, RTLD_NOW | RTLD_GLOBAL)`，验 ABI，
再调用 plugin 导出的 `uniorm_plugin_register(registry&)` 让它把 scheme 装上。
"只用一家"的部署因此连不上其它 backend 的驱动映射，也无需在构建期选组件。
`<plugin_dir>` 默认为 `UNIORM_PLUGIN_DIR` 编译期常量（top-level 构建时被设成
`${CMAKE_INSTALL_FULL_LIBDIR}/uniorm/backends`），运行期 `UNIORM_PLUGIN_PATH`
env 可覆盖成单一目录，用于测试和非常规部署。

拆分与 dlopen 协同的三条硬约束：

- backend 必须是 shared 而非 static：静态归档成员在没人引用符号时根本不会被
  解出，`dlopen` 也拿不到 handle；只有 shared library 才谈得上运行时按名字加载；
- plugin 必须导出 `extern "C"` 的两个符号：`uniorm_plugin_abi_version()` 返回
  编译期常量 `UNIORM_ABI_VERSION`（`<major> * 1000 + <minor>`），
  `uniorm_plugin_register(registry&)` 在传入的单例上调用 `register_backend`。
  ABI 值不等时 loader 拒绝调用 register 并 `dlclose`；错误消息里带着 plugin
  报的 ABI 与核心期望的 ABI，避免半装状态；
- loader 只在 scheme 未命中时触发，且成功后不再 `dlclose`：backend 里的
  vtable / 静态成员地址会活过 `create()`，进程生命周期结束前不能卸载。同一
  scheme 在两个线程上同时未命中时，`dlopen` 的第二次 handle 只是 refcount
  加一，`register_backend` 会因重复 scheme 抛 `backend_error`；`create()`
  把这颗异常当作"另一线程已装好"处理，重新查一次 map，两条路径都拿到同一份
  `factory`。

`UNIORM_BACKEND_ODBC=OFF` 时 `backends/odbc/` 整个子目录不参与构建，也就没有任何 plugin
文件装到 `<libdir>/uniorm/backends/`：`registry::create` 遇到 `odbc://…` 或裸 DSN
时 loader 找不到 `libuniorm_odbc.so`，抛 `unknown_scheme` 并在 message 里附上
`dlopen` 的失败原因（`No such file or directory` 一类），消费者若之前用
`UNIORM_PLUGIN_PATH` 指过别的目录，也能从这条消息看出 loader 究竟找了哪里。
`uniorm_odbc_unit_tests`、集成测试、perf、`uniorm-gen` 都不生成，
`UNIORM_BUILD_TOOLS` 直接被 CMake 拦下；`uniorm_unit_tests` 只链核心，
`test_backend_registry` 因此断言 `"odbc"` 未注册——这条断言在 ON / OFF 两种构建里
都成立，不再需要按选项加宏。

### 5.2 backend 接口（v2 里程碑 1，已实现）

接口位于 `include/uniorm/backend/backend.hpp`，三家实现都不在核心库里：ODBC 是自己
一份 `libuniorm_odbc.so`（`backends/odbc/backend.cpp`，见 §5.1），Oracle OCI 是自己
一份 `libuniorm_oracle.so`，libpq 是自己一份 `libuniorm_postgresql.so`（同见 §5.4）。
核心 API（查询
构建器、映射、池、事务）只依赖接口；能力缺失时应当抛清晰错误而非静默降级。
`capabilities` 上只有两条真分岔——
`columnar_batch` 与 `array_rowcount_totals`——各有一条更慢的退路可走，其余缺什么
抛什么（见下方能力清单）。

**中立列缓冲契约**——三条物化路径（result_set / 聚合投影 / 实体直绑）
统一为"调用方缓冲 + indicator"：

```cpp
namespace uniorm::backend {

inline constexpr std::int64_t null_indicator = -1;  // == SQL_NULL_DATA
inline constexpr std::int64_t no_total       = -4;  // == SQL_NO_TOTAL

enum class buffer_type { bit, int8, int16, int32, int64, float32, float64,
                         chars, bytes, timestamp_parts, date_parts, time_parts };
struct column_buffer { buffer_type type; void* data; std::size_t capacity;
                       std::int64_t* indicator; };
```

`timestamp_parts` / `date_parts` / `time_parts` 为定宽结构，布局钉死对应
`SQL_*_STRUCT`（ODBC adapter 内 `static_assert` sizeof 相等）。ODBC 实现
`bind_column` 即一次 `SQLBindCol`，indicator 直接复用调用方 `int64` 存储
（`sizeof(std::int64_t) == sizeof(SQLLEN)` static_assert），直绑零拷贝的
性能特性不损失，每行仅一次虚调用（`fetch()`）。

缓冲说的是目标结构，不是列的形状：实体字段是 `timestamp`，绑出来的就是
`timestamp_parts`，哪怕这一列只存一半（PostgreSQL 的 `TIME` 只存时钟）。缺的
那半按 ODBC 驱动的读法补齐——时钟无日记为 1970-01-01，日无钟记零时；拿列的
文本硬套完整的日期格式，会把时钟的位数读成年月日，折出一个非法日期。

**语句与连接接口**：

```cpp
struct capabilities { bool columnar_batch, array_rowcount_totals; };

struct statement_iface {
    void prepare(std::string_view sql);                    // SQL 用 '?' 占位符
    void bind_parameter(std::size_t index, sql_value const&);   // 1-based
    void bind_params(params const&);                       // 非虚：按序转 bind_parameter
    void bind_column(std::size_t index, column_buffer const&);
    void bind_batch_params(std::vector<params> const&);    // 行式批量，backend 自行转置
    batch_writer_iface& prepare_batch();                   // 列式批量，直接写缓冲
    void execute();
    bool fetch();
    std::size_t affected_rows() const;
    std::vector<column_info> column_meta() const;
    void set_row_array_size(std::size_t);
    std::size_t rows_fetched() const;
    std::size_t result_row_estimate() const;               // 默认 0（未知）
    void set_paramset_size(std::size_t);
    std::string read_long_text(std::size_t column, std::size_t row);
    std::vector<std::byte> read_long_bytes(std::size_t column, std::size_t row);
    void reset();
};

struct batch_writer_iface { std::size_t add_column(buffer_type, count, element_size);
                            void* data(col); std::size_t element_size(col);
                            std::int64_t* indicators(col); void finish(); };

struct connection_iface { /* open(connection_string)（必选）/
                            open(connection_spec)（可选，默认抛）/ close /
                            is_open / set_autocommit / commit / rollback /
                            caps / dbms_name / create_statement / schema() /
                            native_handle */ };

struct column_shape { /* name / type / nullable；table_shape = 它的 vector，
                        find_column(shape, name) 按目录顺序找 */ };

struct schema_meta { /* 住在 public 的 <uniorm/schema.hpp>，命名空间 uniorm
                      （和 sql_value / params 一样是跨边界的 uniorm 类型）：
                      database_name() / tables(catalog, schema) /
                      table_columns(ref) → {shape, type_name, size, decimals,
                      default_value} / primary_key(ref) / foreign_keys(ref) /
                      indexes(ref)；ref = {catalog, schema, name}，空
                      catalog/schema 表示不限定；非虚的 shape(ref) 把
                      table_columns 的结果收成 table_shape；经
                      connection_iface::schema() 取得（默认 nullptr = 不提供），
                      门面侧是 orm::schema() */ };
```

`open` 有两个形状。必选的那个收连接串里 scheme 之后的尾巴（裸 DSN 串就是整条），
core 从不读它；可选的那个收 `connection_spec`，声明住在
`include/uniorm/connection_spec.hpp`——和 `params` / `schema_meta` 一样，它是跨
边界的 `uniorm` 类型，由 `backend.hpp` 反过来 include：接口点名的类型得住在
`backend.hpp` 那一层或以下，而 `connection.hpp` 在它之上，写 backend 的人不该
为了读这四个字段去包整张连接门面。四件是 `scheme` / `target` / `user` /
`password`，**拼词归 backend**：`scheme` 是 core 唯一读取的字段，它选定
backend；`target` 的语法属于那一家（一个已注册的 DSN 名、`host[:port]/dbname`、
一个 service 串），core 既不认得也不校验。ODBC 侧把四件套拼成

    DSN=<target>[;UID=<user>][;PWD=<password>]

空的凭据整个键省掉而不是送空值，值里出现 `;`、`=`、`{`、`}` 或首尾空白时按 ODBC
规则裹上花括号，括号内的 `}` 写作 `}}`。
`backend_connection::open(connection_spec)` 拼完仍走 `open(std::string_view)`
那条唯一入口：开连接之后要做的事（比如认驱动）跟怎么起的头无关。

不提供这条路的 backend 什么都不必写：默认实现抛 `capability_not_supported`，
和 `exact_tables` 那些可选能力同一个形状。反向不成立——串是原语，四件套是糖：
DSN-less 的串（`DRIVER=` / `DATABASE=` / `Trusted_Connection`）根本没有对应的
四个字段可填，`port` / `dbname` / TLS 策略 / 登录超时这类也一律留在串里。
注册表侧因此只有两条入口：`create()` 解析串取 scheme，`create_for_scheme()`
直接用 spec 的 scheme（同样先归小写，两条路线落在同一个 key 上；scheme 为空时
抛 `unknown_scheme`）。多一个虚函数槽本要让 `UNIORM_ABI_VERSION` 进一格
（§5.1）；这一轮版本留在 `0.4.0`，未进。

列的形状单独成类型：目录读的 `column_row` 还带着服务端自己的类型拼法、宽度和
默认值文本，这些只有生成器留存；比对用的三项（名字、类型、可空）抽出来之后，
`orm::validate` 读 `shape()`，`uniorm-gen` 的列模型直接嵌一个 `column_shape`，
同一份字段只拼一次。声明侧的对应物是 `column_meta`，它带的是成员能绑定的类型
**集合**而非单个类型，所以比对有方向：形状是事实，映射是断言。

旧目录接口保留原有宽松规则：ODBC 结果中的 catalog/schema/table 与所问名称
只差 ASCII 大小写时保留，任一项为空也保留。它排除了部分 pattern 扩展出来的
无关行，但不能区分同一命名空间里的大小写双表，也不能保证特殊字符名称完整匹配。
ODBC 各参数是否为 pattern 要分别处理，不能一概当作通配模式。

逐名解析只使用新增的 `exact_tables(catalog, schema)` 与
`exact_table_columns(table_ref)`。前者完整枚举精确命名空间，后者只接受完整身份
相符的表列；默认实现抛 `capability_not_supported`，不退回旧的宽松接口。
ODBC 在丢弃原始行的身份字段前逐字筛选，必需身份缺失或截断时报错；pattern 参数
按 `SQL_SEARCH_PATTERN_ESCAPE` 转义 `%`、`_` 和转义字符。空 schema 仅表示
MySQL 一类本来没有该层级的命名空间，不是通配符。匹配器再去重完整表身份、检查
矛盾列元数据，一次解析只枚举一次目标范围、每张实际表只读一次列。

ODBC 实现的当前能力：`{columnar_batch=true, array_rowcount_totals=开连接时按驱动
探测}`。两个标志各有真实消费点：前者让 `orm` 的三个批量入口据此在列式与行式通道间
二选一（§4.5.2），后者决定两条受累行数 sweep 的分批粒度（§4.5），ODBC 实现按
`SQL_DRIVER_NAME` 认出 psqlODBC 才清零它。每个标志只表示"有一条更快的路"：缺能力时
核心走慢的那条而不抛错，所以 backend 全置 `false` 也不失正确性。表结构自省不在这批
标志里：它没有更慢的那条路可退，答不出即由 `connection::schema()` 抛
`capability_not_supported`（门面侧的 `orm::schema()` 只转发，§4.7）。清单只装核心
真会分岔的地方：`streaming` / `async_io` / `copy_protocol` / `notifications` 四个
标志位曾按 libpq/OCI 的特性集预留在此，全仓零读者，而它们要命名的特性按 §5.3 走
`native_handle`、不经过核心，占位因此无处兑现。

事务的 autocommit 开关逻辑留在核心（`transaction` 不变），backend 只暴露
原语。`reset()` 的契约是缓存复用：某条 SQL 文本再次从缓存交出时、重新绑定之前调用，
要求只到"关掉游标 + 丢掉该语句自己的簿记"为止——槽位集合由 SQL 文本决定，重新绑定
逐槽位替换旧绑定，所以 ODBC 实现只做 `SQLFreeStmt(SQL_CLOSE)` 并把块/批大小复位
（见 §4.1）即是合规的。接口里原先另有一个 `reset_parameters()`（显式
`SQL_RESET_PARAMS`），核心从不调用它，已删除；一个在重新绑定时不会替换旧绑定的
backend，要在自己的 `reset()` 重写里清干净。

**backend 选择：连接串 scheme + 运行时注册表**（`backend/registry.hpp`）：

- `odbc://DSN=x;UID=u;PWD=p` → "odbc" backend 收到尾串原样进
  `SQLDriverConnect`；
- 无 scheme 的裸串（`DSN=...`）默认 ODBC，保持向后兼容；候选 scheme 须
  匹配 `[a-z][a-z0-9+.-]*`，否则视为裸 ODBC 串（兼容 `SERVER=tcp://host`
  这类怪串）；
- `connection_spec` 不经过解析：`create_for_scheme()` 直接拿它的 `scheme`（同样
  先归小写，两条路线落到同一个 scheme 名上），scheme 为空即抛
  `unknown_scheme`——没有别的线索可选 backend；
- 注册表为 Meyers 单例 + mutex；`register_backend` 重复注册抛 `backend_error`；
  `create()` 在 scheme 未命中时调 `plugin_loader::try_load_plugin`，仍拿不到
  factory 才抛 `unknown_scheme`，message 里附上已注册 scheme 列表与 loader 的
  失败原因；
- ODBC backend 不自注册：注册发生在 `dlopen` 之后由 `uniorm_plugin_register`
  显式调用，进程没走到那个 scheme 就不会载入那一家库。未装任何 plugin 的部署
  里 `"odbc"` 未注册，连接一律 `unknown_scheme`；拆分与 loader 的三条硬约束
  （shared、`extern "C"` plugin 契约、成功后不 `dlclose`）见 §5.1。

**错误体系**：`backend_error : uniorm_error`（backend 名 + context +
`diagnostic{state, native_code, message}`）；`odbc_error` 为其派生
（backend 名固定 "odbc"），故现有 catch 站点不受影响；另有
`capability_not_supported` 与 `unknown_scheme`。

**构建门禁**：`option(UNIORM_BACKEND_ODBC ON)`；启用时 `backends/odbc/` 出一个独立
shared 目标 `uniorm::odbc`，PRIVATE 链 `ODBC::ODBC`（§5.1）；`uniorm-gen` 向
`orm::schema()` 要活库元数据，而目前只有 ODBC backend 提供它，故
`UNIORM_BUILD_TOOLS` 依赖该选项。`option(UNIORM_BACKEND_ORACLE_OCI OFF)` 按同一形状出
`uniorm::oracle_oci`（scheme `"oracle"`，PRIVATE 链 Oracle Call Interface）；配置它要有
一份 Oracle 客户端库，流水线为那一支现装，多数构建机上仍没有，故默认关闭。

**异步不进这份契约**（§4.11）：`connection_iface` 一个虚槽也没为它加。两条理由：
这一层没向任何一家要新东西——它跑的就是那些同步调用，塞进接口等于要每家再实现
一条通路而谁也没多得到什么；另一个理由是队列守着的是核心的生命周期（`~orm()` 的
drain、`~async::transaction` 的兜底回滚），backend 若自起线程去续那条链，就与它抢
同一份所有权。日后某家要用原生异步替掉这一层，形状已经按"调用方一行不改"切好。

### 5.3 原生特性通道

**原生句柄逃生舱口**——保证"ODBC 做不到的事永远有路可走"：

```cpp
auto* pg = conn.native_handle<PGconn>();   // 调用方命名期望的原生句柄类型
PQputCopyData(pg, ...);                     // 用户自行驱动原生操作
```

连接与事务生命周期仍由 uniorm 管理；原生操作发生在借出的连接上，归还前状态必须自洽。

**表结构自省不属于逃生舱**：它走中心入口上的具名访问器 `orm::schema()`（§4.7），
后者转给底层连接的 `connection::schema()`，返回 `schema_meta`（§5.2，public
头 `<uniorm/schema.hpp>`，命名空间 `uniorm`）。每个关系库的 catalog 都答得上
这六条读取，它是可移植的必备能力，不是某家 backend 的私有特性。接口里原先有个
按 `std::type_index` 找 backend 私有对象的 `extension()` 钩子，它把 `void*`
静态转回来且不核对类型；这份读取契约一度挂在它上面（当时叫
`backend::schema_metadata`），现已挪出、钩子已删。COPY / LISTEN-NOTIFY / OCI
数组绑定这类真不可移植的特性因此暂无封装，走上面的原生句柄；要不要类型化的
门面，按 §5.4 落地的进度一家家再定——OCI 那家已经在树里，它暂未用到这种门面。

### 5.4 已确认的 backend 优先级

1. **Oracle OCI**——**已落地**：`backends/oracle_oci/`，scheme `"oracle"`，装成
   `libuniorm_oracle.so`，CI 为它单开一支作业（§8）。它接四件套，也只实现
   `open(std::string_view)` 那条原语；两件 OCI 专有特性还没接——原生数组绑定
   （§9 第 3 条剩下的那一半）与目录读取，后者让它对 `orm::schema()` 报
   `capability_not_supported`
2. **libpq**（PostgreSQL）——**已落地**：`backends/libpq/`，scheme `"postgresql"`，
   装成 `libuniorm_postgresql.so`，CI 为它单开一支作业（§8）。两条 `open` 原语都接：
   整串按 libpq 自己的 conninfo 语法交给 `PQconnectdb`，四件套拼成它命名各值的关键词。
   它交出的两个能力标志也都是 `false`——一批就是逐行 execute 再相加，走的正是核心
   那条更慢的退路。

   值在两个方向上都是文本：绑出去时按列的形状写（`DATE` 不带时钟、`TIME` 不带日），
   读回来时按 `PQftype` 报的形状拆。所以一连上就先钉住两件会话默认——
   `DateStyle = 'ISO, MDY'` 与 `bytea_output = 'hex'`：读的形状是照这两件假设的，
   钉不住就宁可拒绝这条会话。语句按 `uniorm_stmt_<n>` 这条各自唯一的名字
   `PQprepare`，参数类型从 `PQdescribePrepared` 读回；会话把名字弄丢了（SQLSTATE
   26000）就把名字重做一次、再执行一次。`?` 翻成 `$n` 的那趟改写跳过字面量、定界名、
   dollar-quoted 体、嵌套块注释与 `E''`，成对的 `??` 则是留给服务端自己那批 `?` 算符
   （jsonb、几何）的写法。

   它暂未接的是目录读取：`orm::schema()` 对它报 `capability_not_supported`，
   `validate()` 与 `resolve_identifiers()` 在这条腿上便没有可读的目录。大小写因此只剩
   `dialect::detect` 从 banner 答的那个默认（PostgreSQL 答 `lower`），要别的拼法得自己
   `identifier_case(...)` 声明——这条腿与 ODBC 那家的差别就在这里。COPY /
   LISTEN-NOTIFY / 原生异步仍旧走原生句柄，暂无门面。

两者均来自既有项目中必须绕开 ODBC 的实际经验。四件套（§5.2）正是为它们留的路：
libpq 把它拼成空格分隔的 conninfo，OCI 连串都没有——用户名与口令是
`OCISessionBegin` 各自的实参。落地的那家仍只需实现 `open(std::string_view)`
那条原语，四件套按需接。能力清单不为特性集预留标志位：
一家 backend 落地后，只有核心真需要分岔的地方才进清单（§5.2）——OCI 与 libpq 两家交出的
两个标志如今都是 `false`，走的正是各自那条更慢的退路。

## 6. uniorm-gen 代码生成工具

### 6.1 形态

独立 CLI，schema 来源两种：活连接目标数据库，或一份建表脚本（§6.2）。本仓库只
提供可执行文件（`UNIORM_BUILD_TOOLS=ON` 时构建，因活连接那条路要的 schema 自省
目前只有 ODBC backend 提供，故依赖 `UNIORM_BACKEND_ODBC`；顶层构建时也按 §3.1
装进 `<bindir>`）；仓库内没有任何 `add_custom_command`，"构建期生成"要调用方自己
在 CMake 里接。

```
uniorm-gen (--dsn=<dsn> [--user=<u> --password=<p>]
           | --connection-string=<str>
           | --input-ddl=<file>)
           --out=<dir>
           [--config=<file>]          # TOML，见 §6.4
           [--tables=a,b,c]           # 逗号分隔的表名过滤
           [--catalog=<c>] [--schema=<s>]   # 名字，不是 ODBC pattern
           [--name=<n>]               # 产物/命名空间名，缺省取 backend 报的库名
```

三种输入必须选且只选一个，`--out` 必填，否则打印 usage 并返回失败。`--catalog`
/ `--schema` 的值里有 `%` 时照常读取，但先打一条警告：目录按名字问，那个字符不
参与匹配（§5.2）。`--input-ddl` 一路不开连接：服务端有没有、脚本与它差多少，这一
趟都不问（§6.2）。

### 6.2 Schema 提取（走 `orm::schema()`）

CLI 开一个公开 `uniorm::orm`，向它取 `schema()`（§4.7；它转给底层连接的
`connection::schema()`）；backend 不提供自省即抛
`capability_not_supported`，CLI 原样报错退出（今天的 ODBC backend 一直给）。
`--dsn` 与 `--connection-string` 于是汇成同一个连接：前者交四件套
（`connection_spec{"odbc", <dsn>, <user>, <password>}`），拼词与转义归 ODBC
backend（§5.2）；句柄层里那条 DSN 专用的 `SQLConnect` 通路失去最后一个调用者，
随之删掉。

`--input-ddl=<file>` 走另一头：脚本先解析成一份内存目录，再由它答上面这六个读。
读侧的接口只有这六个（`database_name` / `tables` / `table_columns` /
`primary_key` / `foreign_keys` / `indexes`），`read_schema`、`check_config`、
`generate_header` 一段不改，活连接与脚本两份输入自那之后是同一条路。

分工：backend 只回答"目录里有什么"，留下什么、怎么摆由 `uniorm-gen` 决定。

- `tables(catalog, schema)` → 表清单，按 `--catalog` / `--schema` 限定，表名不限定。
  ODBC 实现读 `SQLTables`，只留驱动用来表示"普通表"的那个词（`TABLE` 或
  `BASE TABLE`），视图与系统对象就此挡在生成物之外
- `--tables` 是**客户端筛选**：先精确名匹配，全库无同名时再退一次大小写不敏感
  匹配（`lower_case_table_names` 的服务器），两边都对不上就抛 `uniorm_error`
- `table_columns(ref)` → 一个 `column_shape`（列名、归一后的 `sql_type`、可空），
  外加只有活读才有的：服务端自己的类型拼法、`column_size`、`decimals`、默认值
  （ODBC 侧为 `SQLColumns`，`DATA_TYPE` 的归一发生在它那一侧）。生成器的列模型
  就嵌着这个形状，不再重拼那三项
- `primary_key(ref)` → 主键列名，生成物里标 `.primary_key()`。这是与列清单**两次
  独立的读取**，名字逐字精确比：某个主键名在该表列里找不到，则那一列照常生成、
  不带主键标记，另打一条警告点名它（两份读取只差大小写时警告里带出该表的实际拼
  法）。宁可响一声：静默丢掉主键标记的产物编译得过、跑得通，只是键错了
- `foreign_keys(ref)` → 一个列对一行（ODBC 侧为 `SQLForeignKeys`，按被引用表的
  键序），gen 按被引用表首次出现的顺序分组还原成约束
- `indexes(ref)` → 一个索引一行（列序 + 唯一性，ODBC 侧为 `SQLStatistics`），
  gen 按索引名排序
- `database_name()` → 缺省单元名（ODBC 侧即 `SQLGetInfo(SQL_DATABASE_NAME)`）

每表四次读取。外键与索引都只以注释形式进生成物
（`// FK: col -> pk(col)`、`// index: name (cols)`），无开关。

#### 脚本输入（`--input-ddl`）

`tools/uniorm-gen/ddl_meta.{hpp,cpp}` 把一份建表脚本读成上面那六个问题的答案。
收 MySQL 与 PostgreSQL 两家的写法：`CREATE TABLE` 的列定义与表级
`PRIMARY KEY` / `UNIQUE` / `KEY` / `INDEX` / `CONSTRAINT … FOREIGN KEY`、
`CREATE [UNIQUE] INDEX`、`ALTER TABLE` 的加列与加键。认得出但不改变映射的东西先
计数再跳过，末尾并成一条（`ignored 2 x ALTER TABLE t DROP action`；表级 `CHECK`
和 `FULLTEXT` / `SPATIAL` 同属这一类，键里带上表名）；读不下去的是 `ddl_error`
（继承 `uniorm_error`），带 `ddl: line N:` 指出断在哪一行。

- **名字限定**：一段是表名，两段是 `schema.table`，三段是
  `catalog.schema.table`。脚本说不出一个未限定的表住在哪个 catalog/schema，
  于是它答任何 `--catalog` / `--schema` 限定——否则"一加限定就筛空"。
  `USE x` 答 `database_name()`，没有它就答空串，单元名照旧由 `--name` 给。
- **类型拼法**：`type_name` 就是脚本自己写的那个词（大写归一），`[types]`
  覆写的键和生成物注释因此对得上用户写的；限定类型只取最后一段
  （`public.citext` → `CITEXT`），与活目录报的一致。
- **认不出的类型**照活读的老路走：`sql_type::other` 加生成器那句
  "unmapped SQL type '…', using std::string"。数组在解析期就拒：v1 的绑定集
  不携带数组（§4.3）。
- 没名字的 `KEY` / `INDEX` 由列名补一个（`index_a`、`unique_code`），生成物
  注释里的每个索引因此都有名字。
- 问一个脚本里没有的表，六个读都答空清单、不报错——与活目录对不存在的表一样
  沉默。

### 6.3 生成物

每库一个头文件 `<out>/<name>_schema.hpp`，整体包在 `namespace <name>`（经
`to_unit_name` 归一为 `[a-z0-9_]`）里，内容为：

1. 每表一个 `struct`：表名 → PascalCase 类名、列名 → camelCase 成员名，
   C++ 关键字/非法起始字符会被改写。**命名规则固定，不可配置**——可配置的
   只有 §6.4 里逐表的 `class` 覆写；两张表折成同一个类名时在写文件之前抛
   `config_error` 点名它们，出路分两种：拼法不同的（`user_accounts` 与
   `user_accounts_`）给其中一张写 `class` 即可拆开，只差大小写的两张共用同一个
   配置键、`class` 会把两张一起搬走，只能 `--tables` 只要其中一张。产物里同一个
   `struct` 出现两次，编译期才响，而工具不该把编不过的东西交出去
2. 可空列 → `std::optional<T>`；
3. 每表一个 `inline void register_<Class>_mapping(orm&)`，外加汇总的
   `inline void register_<name>_schema(orm&)`；
4. SQL 类型 ↔ C++ 类型映射遵循 §4.3 表，可被配置覆写。

进注册调用的表名与列名是**源目录的逐字拼法**，生成期不折叠：拼法是那家目录的事实，
而"该发哪一种"要到连接建立之后才知道，所以它交给连接上的 `identifier_case`：方言
认得的家自己答、部署可覆写，都在标识符出口应用（§4.8）。一份产物因此可以服务
存储拼法不同的两家。

折叠发出的是折出来的那个名字，不是声明的那一个：库里同时存着 `"Users"` 与
`"users"` 两张时，声明 `Users` 在 `lower` 之下读到的是小写那张，`validate()` 也只
说命中。这一家唯一的出路是 §4.8 的逐名解析——它按目录里的实际拼法装映射，不受
策略影响。

生成物只输出到指定目录，视为不可手改。头文件仅依赖
`<uniorm/orm.hpp>`、`<uniorm/mapping/registry.hpp>`、`<uniorm/decimal.hpp>`
与标准库，单独 include 就能编译——`orm.hpp` 才给出 `uniorm::orm` 的完整类型，
`registry.hpp` 只前向声明它。

### 6.4 配置文件（TOML）

```toml
[types]                              # 全局 SQL 类型 → C++ 类型覆写
"NUMERIC(10,2)" = "std::string"      # 无损读法；取值限于 §4.3 的可绑定集合
"BLOB"          = "std::vector<std::byte>"

[tables.t_user]
class = "User"                       # 类名覆写
skip = false

[tables.t_user.columns.status]
cpp_type = "std::int16_t"            # 单列类型覆写（同样限于可绑定集合）
converter = "Status"                 # 域类型：成员生成为 Status，绑定走 converter<Status>
```

生效范围：`class` / `skip`；`cpp_type`（含 `[types]` 全局覆写）限于 §4.3 的可绑定
集合，集合外的取值被 `check_bindable` 拒为 `config_error`；`converter` 命名的域类型
原样生成为成员类型，生成物同时以 `static_assert(uniorm::has_converter<...>)` 要求
特化存在，因此使用它的 TU 必须在包含生成头之前声明 `uniorm::converter<Status>`
（§4.4）。converter 只有逐列入口，没有全局覆写。库自带的类型不必如此——`cpp_type =
"uniorm::decimal_t"` 用不着消费者额外 include：生成物自己带上
`<uniorm/decimal.hpp>`。

键的匹配忽略大小写：`[types]` 两侧折成大写，`[tables.NAME]` 与
`[tables.NAME.columns.COLUMN]` 两侧折成小写，于是同一份配置对拼法不同的目录
（Oracle 的 `USERS` 与 MySQL 的 `users`）同样适用。表名命不中目录里的任何表、
或某个列块命不中该表任何列，写文件之前抛 `config_error` 点名各段：被静默忽略的
覆写（最坏的是 `skip`）比一次失败更难查。目录里有、但被 `--tables` 筛掉的那张
表只报得出名字、报不出列，它的列块因此不在检查之内。这项检查要把整份目录列一遍，
所以配置里没有 `[tables.*]` 段时就不列。

## 7. 错误体系总览

```cpp
uniorm_error : std::runtime_error    // 基类（error.hpp）
├── column_not_found                 // 动态行按名取值失败
├── type_mismatch                    // value_cast/get<T>/参数归一化失败
├── mapping_error                    // 映射/校验：重复注册、未注册、表列缺失、可空
│                                    // 不匹配、列类型族与成员不符
└── pool_timeout                     // 池获取超时

backend::backend_error : uniorm_error    // backend 层（backend/error.hpp），
│                                        // backend 名 + context + diagnostics
└── odbc::odbc_error                     // ODBC 句柄层（backends/odbc/error.hpp，私有头），
                                         // backend 名固定 "odbc"

backend::capability_not_supported : uniorm_error   // 无路可退的读取遇上做不到
                                                   // 的 backend：表自省、四件
                                                   // 套连接（§5.2）
backend::unknown_scheme : uniorm_error             // 串的 scheme 前缀没注册，
                                                   // 或四件套没填 scheme

gen::config_error : uniorm_error                   // uniorm-gen 的 TOML/类型配置错误
                                                   // （tools/uniorm-gen/config.hpp）
```

所有 ODBC 失败都经 `throw_if_error` 这一处收口抛 `odbc_error`，而它就是
`backend_error`：公开侧按 `uniorm::backend::backend_error` 捕获即可拿
`backend_name()` 与 SQLSTATE 诊断记录，不需要私有头，适配器也不再需要二次翻译。
集成测试 `test_error_reporting` 钉住这条跨层契约。

## 8. 测试策略

除末条安装冒烟是 `cmake -P` 脚本外，下列用例都是编译进 CTest 的 C++ 程序。

- **单元测试**（无数据库，已实现；核心一份，每家启用的 backend 另有一份白盒）：
  `uniorm_unit_tests` 不链接 ODBC——`test_pfr`（字段数探测/展开/concept 负例）、`test_row`
  （value_cast/收窄/optional/文本字面量按需解析成算术目标）、
  `test_params`（值归一化）、`test_converter`
  （converter 优先于枚举与隐式转字符串两支、表示决定读侧绑定与批量暂存、列的可接受
  类型族、按值取槽位的 `from_db` 搬走暂存缓冲区且跨行复用绑定后仍成立）、
  `test_decimal`（字面量解析与 `to_literal` 往返、小数位数不被规格化掉、整数前导零、
  负零、指数/分隔符/多小数点/超 78 位的拒绝、跨 scale 的序与等值、
  `to_double` / `to_int64` 及两者越界、动态行经 `value_cast<decimal_t>` 与
  `converter<decimal_t>` 的读写往返）、
  `test_expression`（谓词 SQL 生成、方言、分页；`identifier_case` 三值是纯函数，
  连同只折 ASCII 字母这一条一起钉住——非 ASCII 名字原样返回，`lower` 也不碰；
  `detect()` 各家折不折也钉在这里，只有 Oracle 顺带答 `upper`）、
  `test_registry`
  （映射注册/populate/read 闭包/错误路径）、`test_orm_crud_helpers`
  （实体 CRUD 的映射级归一：全部键列推断、WHERE 字段解析与 SET 分区、
  `paramset_size` / `row_array_size` 的 0 归一；未连接的 `orm` 上策略读写自洽）、
  `test_orm_validate`（一份假目录实现 `schema_meta`，把 `validate()` 的每条消息逐字
  钉住：strict/lenient、类型族、可空，以及落空时目录侧的拼法——只差大小写才提候选，
  而候选是拿整份目录比的，于是带出它所在的 schema；这份假目录还能照 MySQL/MariaDB 那
  样按不分大小写的名字答列，此时声明的拼法不在清单里而另一种在，表名即使问得到列也仍报
  在表名；缺的列仍报在列的有两种——清单不收、列却读得到的那张表（视图正是这个形状），以及
  两种拼法都在清单里、这一问正是按精确拼法答的；而每个列名都精确命中的映射压根不会去问清单
  （钉住的是这个行为，不是一个补不上的用例）；假目录还数着自己被列了几趟，通过的校验是 0 趟；
  同一份映射与同一份目录，只换 `identifier_case` 就在成败两侧来回）、`test_backend_registry`
  （scheme 解析边界、注册/重复注册/未注册 scheme；本目标只链核心，`"odbc"`
  scheme 断言未注册——拆库后这条断言在 ON / OFF 构建里都成立，无需按选项加宏；
  另钉 `create_for_scheme` 归小写与空 scheme，以及只实现字符串 `open` 的 backend
  被四件套连接时抛 `capability_not_supported`；实现了那一式的则拿回原样的四件
  ——折小写只发生在查表那一步，`target` 里的 `;` 与空的 `user` 一个字符都不动）、
  `test_pool`
  （用一个记录调用的假 backend 驱动 `connection_pool::release`：归还时回滚挂起的
  工作并复位 autocommit，复位抛异常则该连接被淘汰且名额扣回；四件套的池 key 只到
  scheme / target / user，换 password 仍是同一池，而同处同人的串写法另开一池；
  `orm` 也能从串换乘四件套（先交回借到的那条，两边各是一池）；被淘汰后补上的
  连接仍按四件套配方打开——池留的是配方，不是替它拼好的串）、
  `test_async_task`（一个手泵 executor 钉住 `task` / `executor` 本身：没被
  drive 就不 submit、`block()` 与被 `co_await` 各 drive 一次、未 drive 的
  task 析构不留副作用、结果与异常都穿过挂起点、`block()` 重放同一个
  `exception_ptr`、`start()` 的 handler 拿到同一个——按两次 rethrow 落在
  同一地址来比，才证得出没有另包一层；再加同一个 task 上第二次 drive 被拒，
  和一项不碰 task 的：核心池要零条 worker 时在池还没立起来之前就被拒，拒过两次
  那个数还是没变。
  全部确定，不需要线程；那两个 executor 缝住在 `async_executors.hpp`，
  `test_async_orm` 用同一份）、
  `test_async_orm`（`async::orm` 接到同一个假 scheme 上、`run_on` 挂到手泵，
  假连接记下"哪条语句按什么顺序落到了连接上"，于是 FIFO 的先后、在飞的最多
  一个、`submit_overflow` 在调用点响且被拒的那个 task 之后还能 drive、一次
  批量扫描只交接一次，都是数出来的；`submit_limit` 设成 0 就当 1 用，多出来的
  那条排在调用点被拒；会话自己从连接串立起来、不经过 `uniorm::orm`，四件套与
  直接递一把同步对象这两条入口也各立得起来，`map` 与三个旋钮转发过去之后
  `sync()` 读回同一个值，转发的那几行因此不是第二份状态；`sync()` 改过的注册表
  task 那一侧立刻看得见，钉住"只有一份状态宿主"；同一批行从回调形与从 task 形
  写出去，落到假连接上的语句逐字相同，两条路因此不是两份 body；`resume_on` 那条
  看着续体落到点名的线程上；`validate()` 的负面用例也在这层——它调在协程里、
  仍然同步阻塞，钉住"这是承认的阻塞点，不是遗漏"；`begin()` 三条按顺序日志钉
  句柄与提交 / 回滚的先后，另有一条让句柄活过会话，看会话析构先把它回滚、之后
  再 `commit()` 报的是会话没了；`start()` 的 handler 抛出留在自己那一格，会话
  之后照样接下一条；一条链在 `co_await` 处接到被拒的回合时，异常从 body 的
  `catch` 里出来，不穿过跑这一帧的 worker；用到真 worker 的是 `~orm()` 那句等待
  与屏障那两项，摘掉屏障就全红：
  等交接的那项与回调屏障那条一起红（标志在析构返回时还没置上，屏障因此不用第二枚
  计数），`wait_all_operations()` 一项钉它把交出的与回调的都等齐且会话之后
  照样接下一条，另一项钉被 `submit_limit` 拒的那次不在等里（回调永不响，
  屏障照样返回），还有一项不碰线程：一条链夹在两次请求之间时手里没有回合，
  屏障在它还活着的时候就回来，续体泵干才见第二次读；第二路的八项
  也在这层——回调收到真值与空 `exception_ptr`、失败时第一个参数是 `R{}` 而第二个
  与 body 抛出的是同一个对象、回调形的读排在一条 detach 的写之后且只占一格
  （`queued()` 与 `pending()` 一起看）、`submit_limit` 满时抛在调用点且那次回调不
  响、executor 拒收这一格时回调照样响、`on_done` 自己抛出不外溢也不影响下一次请求、
  空的 `on_done` 在调用点就被拒，以及析构屏障那项：回调里挂在一枚二进制信号量上
  等测试放行，`~orm()` 返回时标志必须已经置上）、
  `test_gen_config`（TOML 子集解析正例/错误行号/非法键）、
  `test_gen_output`（命名转换边界 + 生成器快照与覆写/跳表/converter 生成/DECIMAL 列
  以 `cpp_type` 覆写成 `uniorm::decimal_t`/错误路径，含两张表折成同一个类名时在写文件
  之前抛——拼法不同的那对按提示写 `class` 就能过，只差大小写的那对则逐字钉住它报出的
  另一条出路）、`test_gen_reader`（一份假目录钉住主键名与列清单两次读取的三种形状：
  命中、只差大小写、根本没有，警告文本带出该表的实际拼法；另钉 `--catalog` /
  `--schema` 带 `%` 时的告警）、`test_gen_ddl`（一份脚本喂那六个读：两家的类型
  与参数、引号名与三种注释、默认值的各种形状、可空与键、外键（含内联
  `REFERENCES` 与指向后建那张表的延迟）、索引（含无名键补出的名字）、
  `ALTER TABLE` 的加列/加键和被计数的动作、该拒的脚本（数组、`LIKE`、不闭合的
  引号）逐条钉住，末了把脚本目录接进 `read_schema` + `generate_header` 走一遍
  到底）；后四个只在
  `UNIORM_BUILD_TOOLS` 打开时编入（同时定义 `UNIORM_TEST_GEN`），要链的是
  `uniorm_gen_core`——生成器改走公共自省接口之后它自己不再点名驱动，这四个用例
  于是回到不链 ODBC 的目标里；DDL 那份读的是脚本，本就点不到驱动名；
  `uniorm_odbc_unit_tests` 链接 ODBC——`test_odbc_handles`（句柄 RAII）、
  `test_odbc_dsn_string`（四件套在这一家的拼法：空凭据整键省掉，带 `;` / `=` /
  `{` / `}` 或首尾空白的值裹花括号、括号内的 `}` 写作 `}}`）、
  `test_odbc_catalog`（目录行再筛的那条判据是纯函数：只差大小写算同名，`user_orders`
  对 `userxorders` 不算——后者正是 pattern 里 `_` 的通配，两侧任一为空则留）、
  `test_odbc_error_is_backend_error`（`odbc_error` 就是 `backend_error`，
  无需二次翻译）；
  `uniorm_oracle_oci_unit_tests` 链接 Oracle 那一家——`test_oracle_oci_sql_text`
  （`?` 改写成 `:1 / :2` 时按出现顺序编号，落进引号里的那个算文本不算占位：单引号的
  字面量、双引号的标识符、Oracle 那套 `q'X…X'` 的另类引法都各自成段，成对写出的引号
  不结束它所在的那段）、`test_oracle_oci_types`（`NUMBER` 的精度与标度落在哪个
  `sql_type` 上，以及 native 类型码那张表——`SQLT_INT` 的 `NUMBER(1,0)` 是那条形状
  规则唯一被推翻的地方，它归 `boolean`，因为实体里的 `bool` 绑的正是它）、
  `test_oracle_oci_connection`（没有 target 直接拒；`user/pass@host:port/service` 按
  最后一个 `@` 切，连接描述符里不含 `@` 而口令可以，于是这一刀总是落在对的位置；出错
  时消息只带连接那半，用户名与口令一个字也不进去）；
- **集成测试**（已实现，DSN/凭据由 `UNIORM_IT_DSN` / `UNIORM_IT_USER` / `UNIORM_IT_PWD` 指定，连接串按 `UNIORM_IT_BACKEND` 选中那一家的语法拼（默认 odbc，凭据以 `UID`/`PWD` 写进串里），同一份凭据也按四件套连一次——两条路线连到同一处才算对上，而且四件套那一侧不止跑一条语句：目录里认得出这张夹具表（backend 不答目录时这一段自己报 skip）、`orm` 能从串换乘过来、调用方自己拿四件套建的池也借得出连接，一个不存在的 target 则由这一家自己直接拒（ODBC 侧 `SQLDriverConnect` 不带提示，Oracle 侧不认识的 service 回 `ORA-12154`，CI 上都不会等输入）；连不上时 ctest SKIP）：execute/params 往返、动态行（含 `connection::execute` 显式块取行大小 0 退回逐行）、DECIMAL 动态路径（`DECIMAL(20,4)` / `(20,0)` / `(38,0)` 取回精确定点字面量与 `column_info::scale`，按需解析成 double、scale-0 解析成 int64，带小数或超 int64 范围的字面量抛 `type_mismatch`，NULL 仍报 `is_null`）、DECIMAL 映射路径（`DECIMAL(20,4)` / 一个最宽标度列（精度按这一家收得下的写法）/ `(20,0)` 三列配 `decimal_t` 与 `std::optional<decimal_t>` 成员：批量插入与实体读回逐字核对 `to_literal`（含那个最宽标度列第 30 位为 1 的值——没有任何整型或 double 装得下它）、聚合投影、动态行 `get<decimal_t>`、以 `decimal_t` 为右值的 `where(gt(&T::amount, ...))` 落到同一字面量）、聚合投影（含长字符串与 timestamp）、注册实体读手写 SQL（列序打乱仍按列名配对，缺列报 `column_not_found`）、converter 往返（批量插入、实体物化含 NULL、
`in` 谓词、投影、构建器 `set`/批量 update、动态行 `get<T>`）、orm validate（含 strict
的列缺失/可空/类型族三条失败路径）、查询构建器全谓词与分页（末了拿 `build_select()`
的文本比对这条连接自己那家方言的引号；再把策略声明成 `upper` 重发一次——表名跟着升
上去、原来那条小写断言反过来不成立，出口仍只有一处，随后取消声明、交回服务器自己的
折叠）、标识符拼法的活目录一半（`UNIORM_IT_CASE_USER` 这段建表 DDL 不加引号，于是它存成大写还是小写由
服务器自己的折叠决定；用例先读目录拿到服务器实际存的拼法，把表名与两个列名都声明成相
反的拼法。`keep` 下 `validate()` 必须报错，报错的位置却不交给目录的比对规则决定：声明的
名字与服务器存的只差大小写就一定有地方对不上，而对不上的总是表名——有的目录比名字区分
大小写，拿声明的表名问下去问不到表；有的目录比名字不区分大小写，表连同它的列都会答回来，
但校验会先问一遍清单：这一拼法不在、而只差大小写的另一种在，就报在表名，不让顺带约来的列
名冒充错误。于是三
家同一条消息：报"表不存在"并把服务器存的那种拼法当候选带出来，用例也只断这一条，不分腿；
表列能统一折叠时继续验证原策略成功；不能统一折叠时验证原策略仍拒绝，不再提前退出。
两种形状都调用显式解析，随后校验及映射 CRUD 必须成功，并确认声明名没有被改写。
另有 PostgreSQL 大小写双表用例：先唯一匹配小写表，再创建精确拼写的表并重新解析，
从不同数据/列名确认改选正确对象；临时同名表改变搜索路径后仍访问明确限定的表，
无精确候选时报告歧义。每格实测拼法记一行 `note:`；测试代码已加入，真实运行结果
以当次测试输出为准，不据旧 CI 结果推断本次通过）、事务
commit/rollback/析构回滚、批量插入（含空 optional 写 NULL、1500 行跨 `paramset_size` 分批）、批量 update / 批量 remove（实体版按主键与全字段两种 WHERE，含一张复合主键表验证单实体与批量都按全部键列命中、非键行不被牵连，动态版 `orm::update(table)` / `orm::remove(table)`，以及 `query<T>::set/update/remove` 与无 WHERE / 无 SET / WHERE 字段未映射的守卫抛错）、语句缓存（hit/miss 计数、流式 result_set 借出期间并发 miss、清空）、跨层错误上报（驱动失败以 `backend_error` 捕获，核对 `backend_name()`
与 SQLSTATE 诊断）、连接池借还与超时、连接池维护（心跳保活计数、空闲超时驱逐、失败心跳丢弃；"排空"一律轮询等待而非单次采样，因为正被心跳的连接仍计入 `idle_count()`）；
  异步段七条（除第五条与第六条那两把故意从空会话起步的，其余都跑在同一份注册配方
  上——`map_entities(Session&)` 那一个模板对着手里那把对象写，两个面共用；各自只
  在 id 9100..9199 里写，进段出段各清一次，一段死掉不替下一段决定它数到几）：
  第一条是会话里 drive 那五个终端——批量 insert、
  `all`、`one`、`execute_update`、`query<T>(sql, params)`——每个异步读回的行都按
  字段与同步读回比一遍，外加同一句 SQL 走回调形再读一次；那一次不靠任何泵，它等的
  就是析构那一下。唯一对不上的是本会话那次 `execute_update` 之前的那一份快照，用例
  就把这一处钉在名字上。第二条把同一句失败的读递到四个出口——`block()` 抛出、协程
  body 自己接住、body 不接就原样穿过 drive 动作、`start()` 的 handler 与回调的第二个
  参数——每份文本都与同步那一抛的 `what()` 整串相等（另包一层就在这里红），顺带钉
  成功那次 `start()` 不响 handler 而写入照样落库。第三条走 `async::transaction` 的
  三种收场：提交、显式回滚、协程结束时两个动词都没写（兜底那次回滚就发生在这里），
  事后由同步面确认只有提交留下了行。第四条把四个回调形名字按先后排成一串——insert、
  读、update、一次撞主键的 insert、再读、remove——撞键那次的第一个参数是 `0` 而
  第二个非空，队列在它之后照样答得出，每个答案与同步读回逐字段比；这条里析构不是
  唯一的屏障出口——`wait_all_operations()` 在会话还开着时显式取一次，返回后
  计数与行数就已落定。第五条从空会话
  起步：配置走转发的 `map`，`connect()` 与 `disconnect()` 各是一次排在同一把
  FIFO 上的操作。第六条也自己接过连接串，但只写一张表的映射：转发的 `map` 落下的
  那份注册表就是队列里那条语句渲染用的那一份。第七条开两条会话、两个线程各写各的
  （一条协程加 builder 的 `count()`，一条回调形写与读），每次读只数自己那一段的
  id，所以钉的是库最后装着四行，而不是两者怎样交错。集成用例是一份二进制跑三条
  腿，这七条因此每条腿都跑；后续按库加条件
  标签覆盖方言与类型怪癖；
- **性能基准**（已实现，ctest 标签 `perf`，`tests/perf/`：共享的 harness
  `perf_common.{hpp,cpp}` 装 uniorm 那一半，每条腿另有一个 main 与一个
  原生基线文件）：
  连不上库时 SKIP；行数由 `UNIORM_PERF_ROWS` 指定（默认 10000）。
  覆盖批量 insert/update/delete 吞吐，以及三条查询物化路径的对比：实体直绑
  （`query<T>::all()`）、聚合投影（`db.query<Row>(sql)`，含带字符串与纯 POD
  两例）、动态行（`result_set`/`row`/`sql_value`），另含 `one()`/`count()`
  单行延迟。每项取 best-of-3，输出耗时与 krows/s。
  converter 三例（批量插入、实体直绑、聚合投影）与对应的普通字段用例一一配对：
  同表、同列、同字节，只有 `note` 的成员类型从 `std::string` 换成以 `std::string`
  为 `db_type` 表示的域类型，故两者之差即扩展点的开销；该 converter 读侧直接搬走暂存的
  缓冲区，写侧仍拷一次（实体归调用方，不能被消费），故差值是扩展点剩下的净开销。
  实体读回逐行核对 `from_db` 的结果，避免"只测了行数、解码默默失败"的用例。
  每条腿另含**它自己的原生基线**（不经 uniorm，直接调 client）：同名用例
  即配对，比值是原生耗时除以 uniorm 耗时；只有原生一侧办得到的形状，在表里
  uniorm 那栏留空。ODBC 那半是单行 `VALUES (?, ?, ?, ?)` +
  `SQL_ATTR_PARAMSET_SIZE` + 列方向量数组 + 逐值 `SQLBindParameter` 的批量写
  （与 uniorm 在该 backend 上的数组绑定同构，不存在多行 VALUES 或占位符上限
  分批），加 `SQLBindCol` + `SQLFetch` 的全表扫描（对应实体直绑路径）与单行
  `LIMIT 1`（对应 `one()`）；OCI 那半是逐行 `OCIBindByPos` +
  `OCIStmtExecute`（plugin 的批量就是这个形状），plugin 没用上的原生形状
  ——数组 DML、`OCI_ATTR_PREFETCH_ROWS`——单独成行；libpq 那半是一行一条
  `PQexecPrepared`（libpq 不绑数组，批量只能是这个形状），一条语句带一千行
  VALUES 列表那种写法单独成行。用来衡量 uniorm 抽象层的额外开销
- **`uniorm-gen` 端到端**（已实现，`gen_e2e_tests`，连不上库时 SKIP，两条不开
  连接的腿在它前头各自跑完；连着库的那条按两种形状各跑一遍——调用方写好的串，
  与交出去不拼词的四件套，同一份 golden 判两次）：
  夹具表（含 PK/FK/索引/DECIMAL，时间列是三家同名的可空
  `TIMESTAMP`，见 §8）→ 工具带检入的覆写文件
  （`golden/gen_it.toml`，其 `converter` 键让 `note` 生成为域类型，`cpp_type` 键把
  `amount` 生成为 `uniorm::decimal_t`）生成 →
  与 golden 比对，但只比代码：每行 `//` 之后的注释先截掉，整行是注释的那行
  整行删掉，不分哪一格。
  注释记的是连接器怎么拼 `BIGINT`、服务端怎么存默认值，没有消费者会编译它，而它
  按"连接器 × 服务端"每格都不同——留着它就等于把 golden 钉死在一格上，升级一支
  连接器能同时红三条腿。整行删的是两个来源本来就不必一致的元数据：活目录把主键
  也当一个索报告，脚本没说过这句。截完仍逐字节比代码，成员行是固定两个空格的分
  隔符，两边要对齐每一行的起点。另留一组**语义标记**（FK 与二级索引的注释）：
  这两件事在生成的代码里不留任何痕迹，只有注释能证明抽取到了。主键映射调用、可
  空列与 `decimal_t`/`timestamp` 生成的 C++ 类型都是代码，归代码比对看着。之
  所以代码这一层要每条腿都跑，是因为服务端答空一处元数据时生成的头文件照样
  编译、注册、过 `validate(strict)`，MySQL 8.4 上就真发生过主键整列读丢（见
  §8）——它在代码比对里就是 `.column` 撞上 golden 的 `.primary_key`。
  golden 本身被编译进测试，执行注册 +
  `validate(strict)` + 构建器 `count()` + 实体写入与物化读回（读回同时核对
  `decimal_t` 的值相等与按列 scale 还原出的字面量），覆盖"生成 → 编译 →
  注册 → 校验 → 读写"全链路。
- **离线同源**（已实现，同在 `gen_e2e_tests`，它自己不开连接）：同一组夹具表
  以脚本形式检入（`golden/gen_it.sql`，`prepare_schema` 建的那两张、同一个顺
  序），同一份覆写文件走 `--input-ddl` 再生成一遍，按上一条同一套判据与同一份
  golden 比——"脚本抽出来的代码等于活目录抽出来的代码"是跑出来的，不是推出来
  的。
- **CLI 契约**（已实现，同在 `gen_e2e_tests`，同样不开连接）：真调用那个可执行
  文件，逐项钉它的对外面——两个来源同时给、一个都不给都是退出 2 并打 usage，缺
  `--out` 是 2；脚本读不到、`--tables` 指一个脚本里没有的名、列是数组，各是退出
  1，第三条还钉住报错里的行号（`ddl: line 2:`）；映射没有条目的类型与被跳过的
  `ALTER … DROP` 是警告加退出 0；失败的那几趟一个头文件都不写。退出码、stderr
  的形状与"没写东西"只在真跑一次时才存在，进程内的用例看不见它们。这两条离线腿
  排在 DSN 那道门之前，且那道门认它们：离线腿先失败时返回 1 而不是 77，否则一次
  真失败会被 ctest 记成跳过、进而记成通过。
- **安装冒烟**（已实现，`install_smoke`，`cmake -P` 脚本，只在 top-level 且非交叉编译
  时注册，不需要数据库）：`cmake --install` 到 `<build>/install_smoke/prefix` → 核对
  config/targets/头文件/库都已就位，且装出的头文件集合与 `include/uniorm` 逐一相符；
  核对 `<libdir>/uniorm/backends/libuniorm_odbc.so*` 随 `UNIORM_BACKEND_ODBC` 该
  有则有（两级 soname 文件都在）、该无则无；`libuniorm_oracle.so*` 随
  `UNIORM_BACKEND_ORACLE_OCI` 是同一条判据（Windows 下它落在
  `<bindir>/uniorm/backends/`），两家的 export 集也各按自己的选项该有则有。这一趟
  不 dlopen Oracle 那家——跑它的机器上没有 Oracle 可连——证的是装出去的目录正是
  loader 为 scheme `"oracle"` 写下的那一个；在 Linux 上还 `readelf -d` 装出来的
  `libuniorm.so.<major>.<minor>` 断言 `DT_NEEDED` 里既无 `libodbc` 也无
  `libuniorm_odbc`、既无 `libclntsh` 也无 `libuniorm_oracle`（拆分是否真的落到
  链接期，只在这一步说数）→ 按本次构建的形状
  配置并构建 `tests/install/` 这个外部工程（刻意不设 `CMAKE_CXX_STANDARD`，链接
  行始终是 `uniorm::uniorm` 一家，靠 `SMOKE_UNIORM_HAS_ODBC` 决定消费者断言
  哪一侧）：ON 时装出的 plugin 存在，消费者以 `UNIORM_PLUGIN_PATH` env 把 loader
  指到本次 prefix 的 backends 目录，核对调用前 `"odbc"` 未注册、一次裸 DSN
  跨过 `registry::create` 到 ODBC 层（错误类型不再是 `unknown_scheme`）、调用后
  `"odbc"` 出现在注册表里；OFF 时不装 plugin，`UNIORM_PLUGIN_PATH` 指向一个空
  目录，核对裸 DSN 依然 `unknown_scheme` 且 `"odbc"` 从未注册；这一趟同时验了
  plugin 契约里 ABI 校验真的会拦下不兼容的库——`UNIORM_ABI_VERSION` 由
  `PROJECT_VERSION_MAJOR * 1000 + PROJECT_VERSION_MINOR` 导出，两侧编的是同一份
  CMakeLists，若数字不匹配 loader 立即 `dlclose` 并抛 `unknown_scheme`，消费者
  那条"跨过 create"的断言当场翻红 → 再以 `99.0.0` 配置一次，要求被
  `SameMinorVersion` 拒掉 → 最后运行装出来的 `uniorm-gen --help`，它只可能
  经 `$ORIGIN/../<libdir>` 与 `$ORIGIN/../<libdir>/uniorm/backends` 两条
  RPATH 一起载到库与 plugin，故 RPATH 改写一并验了。同一个消费者工程里另有一段
  协程：它自己实现一个 executor，链式 await 交给默认池跑，`block()` 与 `start()`
  各 drive 一次、同一个 task 上第二次 drive 被拒，再立一把 `async::orm` 在没有
  连接时把 `execute_update` 与 `begin()` 各 drive 一次——后两样的函数体
  在 `src/async/async.cpp` 里，故这一段真 reach 到库内导出的会话面符号；
  单测那两层钉的是行为，这一段钉的是外人链不链得上。
- **CI**（已写入 `.github/workflows/ci.yml`；GitHub 上跑绿过的那副形状是三支作业——
  `core` 与两条驱动腿，三支都按作业原样在 ubuntu:24.04 容器里重放过，服务端用的是一只照抄作业
  `services` 块起出的 `mariadb:11`（实测 11.8.9），连 `MARIADB_DATABASE`/`MARIADB_USER`
  生成的授权与 `mariadb-admin ping` 健康门（约 20 s 转 healthy）也一并验了；提交过
  三趟，头一趟卡在 YAML 校验，第二趟作业真跑起来了、`core` 当场抓出一处真漏，第三趟三支作业
  全绿——前两样的账都在下面。此后驱动那一支沿服务端铺开成四条腿、量完又收回两条，收回之后
  又长出第三条（psqlODBC 对 PostgreSQL 17）：这副四支作业——`core` 加三条腿，四个
  实例——的形状先在 runner 的同族镜像里
  按作业原样重放过，随后就在 runner 上跑绿了——四支作业全绿，三条驱动腿各自跑满除 `perf`
  外的五条，服务端依次答 8.4.11、`11.8.9-MariaDB-ubu2404` 与 17.11。再往后长出的两支
  都不在驱动那一条上：`core-windows` 是 `core` 的 Windows 孪生，同一份
  `UNIORM_BACKEND_ODBC=OFF`、`UNIORM_BUILD_TOOLS=OFF` 的构建换 MSVC 编、flags 给
  `/W4`，ctest 只取 `unit_tests`——它守的是核心与公共头在 MSVC 下编得干净，plugin 的
  `LoadLibraryExA` 那条路径不在它覆盖面里（没有一支 Windows 作业装 backend）；`oracle-oci`
  是第四支作业，它不进驱动的矩阵，因为环里没有驱动管理器——现装 Oracle 客户端库、按
  scheme 名载 plugin、对一只 Oracle Free 容器跑同一套集成测试，注册四条 ctest 而非
  五条：它有自己的白盒单测与同一套集成测试顶上，少掉的那一条正是 `gen_e2e`——生成器
  读的是活库目录，而只有 ODBC 那一家答得上，所以这一支把 `UNIORM_BUILD_TOOLS` 关着。
  第五支 `libpq` 也不进驱动的矩阵，理由与这一支同一条：环里没有驱动管理器。它链
  `libpq`、按 scheme 名载 `libuniorm_postgresql.so`，服务端用的是 psqlODBC 那一条腿
  同一只 `postgres:17`——两条腿读同一个服务端，一个走连接器、一个走原生客户端，
  连接器从前替服务端答的那些东西，差别就在这一支上显出来。它同样关着
  `UNIORM_BUILD_TOOLS`：这一家也还没答目录读取。
  今天 runner 上的形状是五支作业定义、七个
  实例，服务端多答一家 `Oracle AI Database 26ai Free Release 23.26.3.0.0`）：
  一支 `UNIORM_BACKEND_ODBC=OFF`
  的构建只跑 `unit_tests`，它替 §3 那条"驱动类型不漏进 statement 层之上的公开头"把关。
  `install_smoke` 不在这支上，runner 上真跑它的是三条驱动腿、Oracle 与 libpq 那两支：
  前者是
  `UNIORM_BACKEND_ODBC=ON` 的一侧，plugin 装进 prefix，消费者把 loader 指过去便跨过
  `registry::create`；后者正好是 OFF 的一侧，`UNIORM_PLUGIN_PATH` 指到空目录，裸 DSN
  依旧 `unknown_scheme`、`odbc` 从头到尾没进注册表。libpq 那一支则按 `LIBPQ=ON`、
  `ODBC=OFF` 装出 `libuniorm_postgresql.so`，同一趟里三家各按自己的选项该有则有、该无
  则无，`DT_NEEDED` 那道门也多两条断言：核心既不许链 `libpq`，也不许链这一家的 plugin
  ——它只由 dlopen 载入。ON 与 OFF 于是各有作业在守——这条
  负向承诺此前只在注释里，没有任何东西在守它。另一支是三条腿，每条拿自家的连接器连自家的
  服务端，各对自己的服务容器跑除 `perf` 外的全部五条：MySQL Connector/ODBC 取自 MySQL
  自己的 apt 组件（Ubuntu 归档里没有它），MariaDB Connector/ODBC 只能从 tag 拉源码构建
  （Ubuntu 任何发行版都不打包它，上游 release 也不带二进制），psqlODBC 是三支里唯一 Ubuntu
  自己打包的那一支——`odbc-postgresql`（实测 16.00），装出 `psqlodbca.so` 与
  `psqlodbcw.so`，按同一 narrow-char 边界注册 `a` 那一支；服务端三格是 `mysql:8.4`
  （实测 8.4.11）、`mariadb:11`（实测 11.8.9）与 `postgres:17`（实测 17.7）。跨着连的那两种
  配法不跑，也不记成欠账：CI 守的是正常用法，一支对着自家厂商都不支持的服务器不属于那种，
  跨着连从来不在支持轴上。历史上唯一那处真读丢恰好撞在矩阵铺开时多出来的那一格里，它的
  取证记在下面的量里——那是一次曝光的去处，不是一笔要还的账。留着三根服务端轴不是因为
  SQL 会长得不一样——`dialect::detect` 对两个 MySQL 线的 banner 给同一套引号与分页，
  对 PostgreSQL 的 banner 给 ANSI 那套默认，而那条 else 分支此前没有任何一条腿真走到
  它——而是因为 `uniorm-gen` 读的是 **服务端答的元数据**：本地拿真 MySQL 服务端跑这套
  测试，第一趟就撞出 MariaDB 连接器
  `3.1.12` 用 `COLUMN_KEY = 'pri'` 问 `information_schema`，而 8.4 把那些列声明成
  `utf8mb3_bin`（区分大小写，实际值是 `PRI`），于是 `SQLPrimaryKeys` 空返回、生成的
  头文件把两张表的主键整列读丢且不报错。当时把它暴露出来的只有 golden 的字节差，
  而跳过 golden 的腿看不见它。同一夹具换 `3.1.23` 无恙，因为它问的是
  `KEY_COLUMN_USAGE` 的 `CONSTRAINT_NAME = 'PRIMARY'`。CI 钉的是源码构建的
  `3.1.23`，复现不了旧连接器，所以这类沉默改由比对本身兜：`gen_e2e` 三条腿都比
  截掉注释之后的代码（见 §5），那处读丢在代码里就是 `.column` 撞上 golden 的
  `.primary_key`。注释从此一处不比，因为它们本就没有跨连接器跨服务端都一样的时候：
  `8.4` 那格连 `DEFAULT NULL` 都不答，Connector/ODBC 把类型名拼成小写。三只镜像
  各带自家的健康检查客户端，`mysql:8.4` 只有 `mysqladmin`、`mariadb:11` 只有
  `mariadb-admin`、`postgres:17` 只有 `pg_isready`——那道门不携口令，授权仍是预检的事——
  故健康命令按 matrix 给；`services` 的 env 三套前缀都写，三个镜像
  各读自己那半、取值相同，端口映射也按 matrix 给，因为两支 MySQL 线听 3306、
  PostgreSQL 听 5432。除 `core-windows` 外都带 `-Wall -Wextra`——它把 flags 换成 MSVC
  的 `/W4`——今天零告警，但不 `-Werror`，免得依赖头升级把与回归无关的红压进分支。跑数据库
  的四支（三条驱动腿与 Oracle 那一支）测试都以 `-V` 跑，因为 §8 那个用例会记下每个名字实际
  存成了什么拼法，而那一行出自一个通过的用例，ctest
  对通过的用例不出它的输出。四支各有一道报警：
  测试连不上就返回 77，而 ctest 把 77 记成 Skip 并照样打印"100% tests passed"，所以作业
  见到输出里的 `Skipped` 即判失败（真正的失败交给 `set -o pipefail`，测试条数不写死）。
  三条驱动腿还在构建之前先用 `isql` 问过是哪台服务端答的：每条腿带一个 `server_prefix`（引号不能省，
  `11.` 裸写会被 YAML 读成数字 11，那前缀任何 MariaDB banner 都对得上），拿**该腿自己的**
  那句版本查询的 banner 去比前缀，对不上即红——问哪句也是服务端自己的选择，PostgreSQL 的
  `SELECT VERSION()` 答 `PostgreSQL 17.7 (…)`，开头是个词不是数字，那条腿因此问
  `SHOW SERVER_VERSION`。这既把"驱动没装对"与"库有回归"分成
  两种红，也不让一条腿连着另一台服务端照样绿——本条目里那个写错的 DSN 端口，就是把一次
  "MySQL 8.4"的探测变成了 MariaDB 上的假"表不存在"。
  这趟按镜像原样的重放换到的比之前所有手工仿真都多，因为**两条腿拿到的驱动都不是先前那两
  支**。apt 给的 MySQL 连接器是 `26.7.1`，不是本地仿真那支 `8.4.0`，而 2031 属于 8.4 那一
  代：同样七种参数形状，26.7.1 在服务端预处理**开着**时对 `10.6.4`、`11.8.9` 两个 banner 与
  `mysqld-8.4.11` 全通，五条测试也全绿，于是那支腿不再需要 `NO_SSPS`，矩阵里那个键退回成
  一段写给 8.4.0 的注释，`dsn_extra` 留作逃生口。它同时回答了这个条目原先留给 runner 的
  未知项：apt 装出的文件叫 `libmyodbc26a.so` / `libmyodbc26w.so`（不沿用 8.4 tarball 的
  `libmyodbc8*.so`，故驱动经 `dpkg -L` 找，不按名字 glob），而 `w` 那一支会把普通 `varchar`
  列渲染成 `SQL_WVARCHAR(-9)`，所以注册的是 `a` 那一支。它与 golden 的注释差照旧
  （`bigint(19)` 外加一句服务端从未存过的 `DEFAULT NULL`），只是这些如今本来就在
  比对之外。
  MariaDB 那条腿的账在版本与装载上：从 tag 构建出的 `3.2.9` 对**数组绑定的六参数 INSERT**
  （实体批量插入那条）回 `(2008) Client run out of memory`，两台服务端一样，而把同一绑定
  形状用裸 ODBC 原样写出来——含混合 NULL、`SQL_C_TYPE_TIMESTAMP`、65 字节步长的 varchar
  数组、显式长度而非 `SQL_NTS` 的 prepare、`SQL_ATTR_ROWS_FETCHED_PTR`、execute 前
  `SQLFreeStmt(SQL_CLOSE)`——在 3.2.9 上全部通过，所以这笔账在驱动侧，不在 `uniorm` 的用法
  上；`3.1.23` 同一趟五条全绿，golden 也逐字节对上了——按那时还要比注释的判据
  （§5 现在不比），这条腿因此钉在 3.1 线上。构建出的驱动与
  它自己链接的那份 `libmariadb.so.3` 并排装进 `/usr/local/lib/mariadb`，安装时 RPATH 又被
  清空，不把该目录写进 `/etc/ld.so.conf.d` 再 `ldconfig`，驱动管理器只回一句
  `Can't open lib ... file not found`——那是 dlopen 的失败，与被点名的路径存不存在无关。
  另三处只有 runner 的镜像才会撞到：`odbcinst` 这个命令行在 Ubuntu 上是独立的一个包，不随
  `unixodbc` 装，作业因此不用它，探测全交给 `isql`；`sources.list` 的一条 `deb` 必须是一个
  物理行，按 YAML 折行会把源拆成两行，apt 静默读不到那个组件；而 MySQL 那个归档的签名 key
  虽仍是同一把 `B7B3B788A8D3785C`，`RPM-GPG-KEY-mysql-2023` 那份副本的有效期已在 2025 年 10
  月过去，apt 于是报 `EXPKEYSIG` 把归档当成未签名而拒掉，要取 `RPM-GPG-KEY-mysql-2025`
  那份续过期的副本（取到后 `apt-get install --reinstall` 确实从 `noble/mysql-tools` 拉回
  `26.7.1`）。
  真跑起来后它头一趟就抓到东西，两样都不是本地能撞到的。其一是 YAML 校验：`services` 块
  拿不到 `env` 上下文（那里可用的一列只有 `github`、`needs`、`strategy`、`matrix`、`job`、
  `runner`），整个文件在排队前就被判 invalid，服务容器那四个口令与健康门用的 root 口令只
  能写成字面量，与作业 `env` 映射的一致性归 `isql` 预检管。其二是 `core` 作业编到
  `src/types.cpp` 就停在 `fatal error: sql.h: No such file or directory`——正是这条契约要
  抓的泄漏：那张 `SQL_*` → `sql_type` 的映射表以 `UNIORM_API` 的资格住在公开头和核心库里，
  而 runner 的镜像不装 `unixodbc-dev`。本地那趟重放是绿的，只因为仿 runner 的容器为了编驱动
  早已把那些头装上了：一条断言的成败取决于某个包在不在，它不配叫守卫。映射表因此搬去
  `backends/odbc/native_types.hpp`（`inline` 头，不进 ABI），`column_model` 改存中立 `sql_type`、
  由 `schema_reader` 当时那条 ODBC 边界归一（那些读取现已挪进
  `backends/odbc/schema_catalog.cpp`），公开头不再声明 `sql_type_from_native`；`core`
  作业另加一道 shadow：往 include 路径最前放一对读下去即报错的 `sql.h`/`sqlext.h`，再用一次
  反面编译确认它们确实抢在了系统头之前——且要求那次编译非报我们那句 `#error` 不可，编不动
  的编译器同样会"失败"，而那不算守卫生效。这笔搬迁之后，除 `tests/perf` 那条裸 ODBC
  基线，`sql.h` 只在 `backends/odbc/` 里出现：`core` 那道 shadow 拦的从此是一个方向，
  不再是某一次具体的回归。两处都改完后再提交一趟，三支作业在 runner 上全绿：
  `core` 带着 shadow 编过，mariadb 腿的 golden 仍逐字节相符——也是按当时的比法。
  全绿之后这支沿服务端铺开成 2×2 四条腿，量完又收回两条——上面那笔取舍就是从这趟下的。
  本地量到的：两支连接器都能连上 `8.4.11`，用户是 `caching_sha2_password`、走 TCP、
  DSN 不需要任何额外键；四格各跑一遍抽取、固定同一份 `uniorm-gen`（那份构建早于
  `decimal_t`），截掉注释后的输出四格 md5 相同——格与格的差别全在注释里，代码一处没有。
  这条规则套今天的工具与今天的 golden
  复算过：MariaDB 那一格五条全绿；`3.1.12` × `8.4` 那一格四条绿（含 `integration_tests`，
  那台服务端开着 `ONLY_FULL_GROUP_BY`），红的一条正是 `.primary_key` 变成 `.column`
  的那处读丢，不必任何开关。红因那支旧连接器而在，也就由 CI 钉的那两支来还：另起一个
  `ubuntu:24.04` 的容器（runner 的同族镜像），apt 装 `26.7.1`、tag 源码构建 `3.1.23`，
  把当前这棵树按数据库腿的形状配置（`UNIORM_BACKEND_ODBC=ON`、`UNIORM_BUILD_TOOLS=ON`、
  `-Wall -Wextra`）跑 `ctest -LE perf`，两台服务端各连一次——四格五条全绿，一条 Skip 也没有。
  四格都绿过一次，留哪两格便成了取舍而不是没得选：留下两条同名腿，拿掉的跨格从此没有
  CI 覆盖——这不是一笔欠账，理由就是上面那条配法，§9 也不把它列成待做。
  第三条腿的账全在 runner 的同族镜像里量，四问都有答案。**包与文件**：psqlODBC 是三支里
  唯一 Ubuntu 自己打包的（universe 的 `odbc-postgresql`，实测 `1:16.00.0000-1`），装出
  `psqlodbca.so` 与 `psqlodbcw.so`，注册的仍是窄字符边界选中的 `a` 那一支，路径经
  `dpkg -L` 找。**DSN 的键**：三支都读 `Servername`，而 psqlODBC 干脆不理 `Server`——
  留着那种写法它对 `127.0.0.1` 无动于衷，转去敲本机 unix socket，回
  `connection to server on socket "/var/run/postgresql/.s.PGSQL.5433" failed`；作业因此
  写 `Servername`，两支 MySQL 线照旧读得懂它，`UID`/`PWD` 三边都透传。**版本**：见上面
  那句 `SHOW SERVER_VERSION`。**`DATETIME` 换成 `TIMESTAMP` 之后驱动报回什么**：夹具原样
  写 `DATETIME`，PG 无此名；改成三家同名的 `TIMESTAMP` 后，三支驱动报回的 `sql_type`
  落进同一个槽，生成的 `created` 字段与 golden 逐字节对上——这是三条腿各自跑完 `gen_e2e`
  得到的，不是推的。可空 `TIMESTAMP` 在 `mysqld-8.4.11` 与 MariaDB 11.8.9 上都因
  `explicit_defaults_for_timestamp=1` 不带隐式默认，`created` 读回空值那条断言照旧成立。
  列式的 `DOUBLE` 一并换成三者同名的 `DOUBLE PRECISION`，MySQL 表语法独有的行内
  `KEY … (col)` 挪成单列一条 `CREATE INDEX … ON tbl (col)`。于是**一份 golden 供三家**：
  生成器在 PG 上的目录调用、主键/外键/索引提取、`decimal_t` 的 scale、`validate(strict)`
  全部对得上，不必另立 golden，也不必把比对按家族放宽。只有 `integration_tests` 里那句
  引号断言得改口——它反引号写死了，而引号是 banner 决定的不是驱动决定的，于是它自己
  `dialect::detect(dbms_name())` 再比；`detect` 的两条规则本就钉在单测里。
  还有一样只有真连 PG 才撞得到，且它撞在库上不在作业上：**数组绑定 UPDATE/DELETE 的
  行数**。psqlODBC `16.00` 把每组参数都执行了，`SQLRowCount` 却停在其中一组的行数——
  5 组全命中它报 `1`，两支 MySQL 线连接器报 `5`（抽掉一组命中：`1` 对 `4`）；逐组
  execute 再累加，三者得同一个数，也没有按参数组计数的诊断可捞（`SQL_DIAG_NUMBER`
  三者都回 `0`）。`update()`/`remove()` 返回的正是这个数，所以核心不能再拿它当数组的
  总数：`caps()` 添 `array_rowcount_totals`，ODBC 实现开连接时按 `SQL_DRIVER_NAME` 认出
  psqlODBC 并清零它，`update()` 与 `remove()` 随即把分批收成 1（§4.5、§5.2）。
  两条替路量过而否掉：
  报绑定的行数会让 `remove()` 对不存在的行撒谎；不分驱动永远逐组 execute 则多付服务端
  命令——`Questions` 的增量量出 MariaDB 的连接器本来把 5 组压成**一条**命令、
  Connector/ODBC 本来就发 **5** 条。列式 insert 数的是绑定的行数，不受影响。
  runner 欠的那笔已经还上：作业自己这副形状——矩阵选镜像、按镜像给端口与那句版本查询、
  `health_cmd` 按镜像给、`services` 里三套 env 前缀，配错一条腿则 banner 预检先红——
  在 runner 上四支全绿，三条腿的服务端分别答 8.4.11、`11.8.9-MariaDB-ubu2404` 与 17.11。
  至于先前那笔 SSPS 与 `NO_SSPS` 的代价对照，量的是 `8.4.0` 对 `8.4.0`（服务端
  `mysqld-8.4.11`，只切那一把）：缓存命中的形状上 SSPS 略优（每语句 0.212 ms 对 0.228 ms，
  数组绑定批量 0.193 对 0.218——驱动得在本地把值格式化进语句文本），每个只出现一次的语句
  文本上反过来（0.476 对 0.295，SSPS 多付的正好是一次 prepare 往返，量级等于一次
  `SELECT 1`）。那是那支连接器上的账，留着只为说明 2031 的来路。同一趟下 ASAN 会抓到
  连接器自己在 `fill_fetch_buffers` 里对 `allocate_buffer_for_field` 分配的 1024 字节
  结果缓冲做 `strlen`（越界 1 字节，正好没有 NUL 位）——那是厂商的账，只在 ASAN 构建下
  显形，别把它当成库的回归。

## 9. v2 路线图

**v1 欠账**（`decimal_t` 随 0.2.0 落地、CI 的三条作业既在它们所要用的镜像里按作业原样重放过
一遍、也在 GitHub 上跑绿之后，本清单一度清空；驱动那一支沿服务端铺开成四条腿、收回两条、
又长出第三条，此后又长出 `oracle-oci` 与 `core-windows` 两支，四支作业定义在 GitHub 上
全绿——本清单如今没有待做项，逐条下落见各条目末尾与 §8）：

- ~~ODBC 宽字符路径（§4.2）~~ **已按该条目自己给出的第二条路了结**：确认不做，
  删掉零调用者的 `utf8_to_utf16` / `utf16_to_utf8` 与只有它们会抛出的公开类型
  `unicode_error`，边界策略写实为"一律窄字符、编码转换留在驱动侧"。何时值得重新
  引入，见 §4.2 末尾的触发条件。
- DECIMAL 无损动态路径（§4.3）：~~动态行按 `SQL_C_CHAR` 取原始字面量、
  `column_info` 保留 `scale`、清掉零引用的 `UNIORM_DECIMAL_AS_STRING`~~
  **已完成**——`sql_type::decimal` 归入 `slot_kind::text`，字面量精确进进程，
  `value_cast` 按需解析回算术目标；`scale` 目前只对外暴露，库内无消费点；
  `UNIORM_DECIMAL_DEFAULT` 连同派生宏一并删除，生成器的 DECIMAL 默认恒为
  `std::string`，要 `double` 走配置的 `[types]` / `cpp_type`。~~**待做**：真正的
  `decimal_t`（尾数 + scale）~~ **已完成**：`uniorm::decimal_t`（尾数 + scale，
  见 §4.3 第 2 条）。它只以 `converter<decimal_t>`（`db_type = std::string`）的
  身份进入三条映射通道，绑定与取值机器一行未改，动态行的存法也未变（仍是字面量
  文本，`value_cast<decimal_t>` 现取）；生成器侧默认仍是 `std::string`，换类型是
  逐列的 `cpp_type = "uniorm::decimal_t"`；
- ~~打包~~ **已完成（§3.1）**：`install(TARGETS/EXPORT)` + config/version 文件 +
  `VERSION`/`SOVERSION`，`$<INSTALL_INTERFACE:include>` 与 `project(VERSION)` 已
  生效，外部工程可用 `find_package(uniorm CONFIG)` 接入，`uniorm-gen` 也随
  `UNIORM_BUILD_TOOLS` 装进 `<bindir>`。~~**待做**：让这套验证在流水线上跑一次~~
  **已完成**：`.github/workflows/ci.yml`（§8）的三条形状都已按作业原样在它所要用的镜像里跑过，这一趟
  把该条目原先留给 runner 的四个未知都收掉了：apt 组件里的 MySQL 连接器确实落地，是
  `26.7.1`，文件名为 `libmyodbc26a.so` / `libmyodbc26w.so`；从 tag 源码构建的 MariaDB
  连接器编得过（`3.1.23` 与 `3.2.9` 都编得过，但后者跑不过套件，见 §8）；golden 在
  mariadb 腿上逐字节成立、在 mysql 腿上只差一处连接器渲染，那笔渲染后来整体退出了
  比对（见 §5）；驱动与 DSN 的
  注册、`isql` 预检、`-LE perf` 过滤后的五条，连同作业那段 `services`（授权与健康门）也
  都在镜像里绿过。GitHub 也已经真跑过它了：头一趟只有 YAML 校验拦下的一件事（`services`
  块读不到 `env` 上下文），改完的第二趟作业起了、`core` 抓出一处真漏并已修，第三趟三支作业
  全绿（两处细节都在 §8）。原先留的两样都已结掉。其一——长出第三条腿后的
  这副形状还没被 runner 看过——已经结掉：驱动腿一度沿服务端铺开成四条腿，
  在装着 CI 那两支连接器的 `ubuntu:24.04` 容器里按当前构建跑满四格全绿，
  随后收回两条同名腿（CI 守的是正常用法，§8），第三条腿是在这副两腿上长出
  来的，它和另两条都在 runner 的同族镜像里按作业原样各自跑满五条（§8）；
  作业自己那副形状（矩阵选镜像、按镜像给的端口、健康门与那句版本查询、
  `services` 里三套 env 前缀）随后在 runner 上四支全绿。另一样是跨格
  （MariaDB 连接器对 MySQL 服务端、Connector/ODBC 对 MariaDB 服务端）从此
  没有 CI 覆盖：那不是要还的账，跨着连不在支持轴上（§8）。此后又长出
  `oracle-oci` 与 `core-windows` 两支，各自也在 runner 上绿着，今天的形状
  是五支作业定义、七个实例（§8）。第五支 `libpq` 与这两支的差别在于它还没在 runner
  上跑过：形状按作业原样在同族容器里重放过，集成的三条腿与安装冒烟都在那里跑绿。
  `uniorm-gen` 的元数据读取已挪到 `orm::schema()` 之后（§6.2）；挪动前后对
  同一份夹具逐字节相同：那格要不要补一道门，看它那时暴露出什么，届时再定。

原有路线图：

1. ~~backend 接口提取~~ **已完成（里程碑 1）**：中立接口 + scheme 注册表，
   ODBC 迁移至接口之后，核心单测不链接 ODBC（见 §5）；它那时还只是核心
   PRIVATE 链的一家，此后整家拆成 plugin（§5.1、条目 8）。三家都已落地，剩下的
   **待做**是 OCI 与 libpq 两家都还没接上的目录读取（§5.4）。
2. Unit of Work / 脏检查 / 级联
3. ~~批量操作~~ **已完成**：`SQL_ATTR_PARAMSET_SIZE` 数组绑定 + 块取行，
   列式（`batch_writer_iface`）与行式（`bind_batch_params`）双通道（见 §4.5.2）。
   **待做**：OCI backend 的原生数组绑定
4. ~~异步包装层（libpq backend 可用原生异步）~~ **第一版已完成**：`async.hpp`
   那一层（§4.11）就是这一条，只是它站在同步契约之上做仿真，一家 backend 也不点
   ——ODBC 侧实测没有可用的原生异步（§4.1），先建在接口上等于给每家加一份它兑现
   不了的账。原生异步那一半仍归各家：libpq 真用得上它时，才是它进 backend 契约的
   理由（§5.2 记了它为什么现在不进）。这一层后面另留了两件独立的事，都不在它里面
   ——池的 awaitable 等待者表（替掉 `condition_variable`，§4.10）与
   `orm::connect()` 握着旧租约时那个顺序（§4.11 末）
5. 外键导航 / 关联加载
6. ~~离线 schema 快照输入（DDL 解析）~~ **已完成**：`uniorm-gen --input-ddl=
   <file>` 收一份建表脚本，由它答 `schema_meta` 那六个读，之后与活连接同一条
   路（§6.1/§6.2）。解析器认的是两家的 SQL 原文，`mysqldump` / `pg_dump`
   导出的建表段可以直接喂进来
7. 迁移脚本生成
8. ~~backend 拆成独立链接目标（如 `uniorm_odbc` / `uniorm_pq`）：多 backend 共存时
   让"只用一家"的部署不必在运行时载入其余驱动（见 §5.1）~~ **已完成**：每家
   backend 一份 `libuniorm_<scheme>.so`，装到 `<libdir>/uniorm/backends/`；核心
   `libuniorm.so` 不再链任何驱动，消费者链接行始终只有 `uniorm::uniorm`——选
   backend 由运行时按 scheme `dlopen` 完成（`registry::create` miss 时经
   `plugin_loader::try_load_plugin` 载入并 `uniorm_plugin_register` 注册），
   三条硬约束（backend 必须 shared、`extern "C"` plugin 契约带 `UNIORM_ABI_VERSION`
   校验、成功后不 `dlclose`）见 §5.1。ABI 校验从 0.4.0 起把 `SONAME` 与
   `UNIORM_ABI_VERSION` 绑到同一份 `major.minor`：核心与 plugin 不同 minor 直接
   拒载，链接期失败不足以覆盖 dlopen 期的错配。`find_package(uniorm COMPONENTS
   odbc)` 那条路已废；`uniormOdbcTargets.cmake` 仍安装，只为仓内白盒测试直链
   `uniorm::odbc` 保留，OCI 那一家照同一形状也有一份 plugin 与一份白盒 export
   （§5.4）。Windows 的 `LoadLibraryExA` 路径也已在 loader 里实现（§5.1），只是
   没有一支 Windows 作业装 backend，它不在 CI 覆盖面里（§8）。libpq 那一家也已按同一
   形状长出：一份 plugin、一份白盒 export（`uniormLibpqTargets.cmake`），CI 也为它单开
   一支作业（§8）。
9. ~~能力清单落地：`capabilities` 的四个未读标志各自找到真实消费点~~ **已完成**：
   四个占位已删，清单只剩 `columnar_batch` 与 `array_rowcount_totals`，两条各有
   读者（§5.2）；后者随 §8 那条 PostgreSQL 腿落地。
   ~~并把 `capability_not_supported` 的抛出接上~~ **已完成**：表结构自省没有
   更慢的退路，backend 答不出时由 `connection::schema()` 抛它（§5.2）
10. ~~池归还时的状态清理~~ **已完成**：`release` 见 `autocommit()` 为假即
    rollback 后复位 autocommit，复位抛异常则淘汰该连接并扣回名额（见 §4.9）；
    兜底不再只挂在 `orm` 借出侧，直接用 `connection_pool::acquire()` 的借用者
    同样拿到干净的连接
11. ~~标识符逐名解析~~ **已实现**：显式 `resolve_identifiers(catalog, schema)`，
    不在 `validate()` 中隐式执行；声明保留，结果单独保存并原子安装（§4.8）。
    首版范围为 PostgreSQL、MySQL/MariaDB 的明确命名空间内普通表。公共布局和
    `schema_meta` 虚表的变化把版本推到 0.3.0 / SONAME 0.3，消费者与第三方
    backend 需要重新编译；不能仅因某个类型的 sizeof 没变就混用旧二进制（此后
    0.4.0 又因 backend 拆分动过一次，见条目 8）。

## 10. 评审待定点

1. ~~聚合投影自实现 PFR 手法还是依赖 Boost.PFR~~ **已定：自实现 pfr-lite，字段上限 64，不引入 Boost**（见 §4.6）；
2. ~~DECIMAL/NUMERIC v1 默认映射~~ **已定方向：默认取无损一侧**（见 §4.3）。
   动态行取精确定点字面量（无损），`uniorm-gen` 的默认映射生成 `std::string`
   成员，实体/投影随成员类型（`std::string` 即无损）；"可配"原先落在 CMake 选项
   `UNIORM_DECIMAL_DEFAULT` 上，但它只改生成物、不改库，已删除——配置改由
   生成器的 `[types]` / `cpp_type` / `converter` 按列承担（§6.4/§4.4）。
   `decimal_t` 也已落地：`uniorm::decimal_t` 以自带 converter 接入，正走的就是这条
   按列覆写（§4.3 第 2 条）；
3. ~~`orm`（注册表）与 `connection` 的组合方式~~ **已定：`orm` 作为中心入口，内部持有 `connection`**，`db.query<T>()`、`db.insert()`、`db.update()` 等统一经 `orm` 调用（见 §4.8）；
4. ~~头文件-only 还是编译库~~ **已定：动态库**（避免 header-only 升级后全量重编），非模板实现进 `libuniorm`，模板代码留头文件（见 §1）。

**四项决策均已定，且都已落到实现。**"决策定了"与"实现到位"此前在第 2 项上分叉
（欠一个 `decimal_t`），如今并拢；仍未做的事只剩 §9 的条目。
