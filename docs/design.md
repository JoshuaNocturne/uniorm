# uniorm v1 设计文档

状态：v1 实现完成（单元测试 + MariaDB 集成测试 + 性能基准通过）
日期：2026-09-09（本版按当前代码逐节核对，偏差集中记于"已知缺口"——该节现已为空）

## 1. 目标与范围

uniorm 是一个基于 **ODBC**（而非各数据库专有 C 客户端）的现代化 C++ 数据库访问层。

### v1 范围（In Scope）

- ODBC 句柄的 RAII 封装（env / dbc / stmt）
- 连接管理与最小连接池（固定大小、借还、超时）
- 事务（RAII）
- 参数绑定、语句执行、结果集迭代
- 批量 CRUD：数组参数绑定（`SQL_ATTR_PARAMSET_SIZE` 分批）+ 块取行
  （`SQL_ATTR_ROW_ARRAY_SIZE`），见 §4.5.2
- 类型系统：SQL 类型 ↔ 现代 C++ 类型映射，含 converter 扩展点（§4.4）
- 三种结果对象：
  - 聚合 struct 投影（按列序/列名绑定，无需注册）
  - 实体映射（成员指针路线，显式注册）
  - 动态行对象（按列名取 variant 值）
- 基于成员指针的类型安全查询构建器
- Schema 校验（利用 ODBC 元数据 API）
- `uniorm-gen` 代码生成工具（活连接、ODBC 元数据提取、TOML 配置）

### v1 明确不做（Out of Scope）

- Unit of Work / 脏检查 / 级联加载 / 懒加载（留待 v2）
- 异步 API（ODBC 本身同步；v1 同步接口，未来可在线程池上包装）
- DDL 文件解析（代码生成仅支持活连接）
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

另有一处较小的偏差体量不足以单列，直接在正文就地写实并进了 §9："不碰 ODBC"
只到链接行为止（§5.1）。

### 基础决策

| 决策项 | 结论 |
|---|---|
| C++ 标准 | C++20 |
| 接口风格 | 纯同步 |
| 平台 | Linux（unixODBC）、Windows（原生 ODBC） |
| 错误处理 | 异常 |
| Unicode | 内部一律 UTF-8；只做窄字符绑定（`SQL_C_CHAR`），编码转换留在驱动侧（§4.2） |
| 库形态 | 动态库（`libuniorm.so` / `uniorm.dll`），经 `UNIORM_API` 导出宏控制符号可见性；模板密集代码（mapping/builder/projection/pfr）保留在头文件 |

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
│ odbc_error / diagnostics                        │  （当前唯一内置实现）
└────────────────────────────────────────────────┘
```

依赖方向严格向下；高层不直接触碰 `SQLH*` 句柄类型。ODBC 专有概念（`SQLLEN` / indicator / SQLSTATE / `?` 占位符细节）禁止上浮到语句层公共 API 之上——该纪律由不链 ODBC 的核心单测目标在**编译与链接期**把守（运行时仍会经 `libuniorm.so` 载入驱动管理器，边界见 §5.1）。

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
│   ├── connection.hpp           # connection：连接前置的 low-level 入口（含语句缓存原语）
│   ├── transaction.hpp
│   ├── pool.hpp                 # connection_pool / pooled_connection / connection_pool_registry
│   ├── dialect.hpp              # 方言特性（引用符、分页）
│   ├── backend/                 # 驱动中立的 backend 契约（见 §5.2）
│   │   ├── backend.hpp          # column_buffer / capabilities / statement_iface / connection_iface
│   │   │                        # / batch_writer_iface（列式批量写）
│   │   ├── registry.hpp         # scheme 解析 + backend 注册表（out-of-tree backend 注册入口）
│   │   └── error.hpp            # backend_error / capability_not_supported / unknown_scheme
│   ├── detail/
│   │   ├── pfr.hpp              # 自实现聚合体反射（字段数探测 + 展开，上限 64）
│   │   ├── projection.hpp       # 聚合 struct 投影绑定（field_binding 体系）
│   │   ├── traits.hpp           # is_optional_v 等共享 traits
│   │   └── time.hpp             # chrono ↔ 日历拆分/组装
│   ├── orm.hpp                  # 实体注册与 CRUD 入口：除模板外只有声明，实现全在 src/orm.cpp
│   ├── mapping/registry.hpp     # 实体映射注册表（含 mapping_builder；uniorm-gen 产物唯一依赖）
│   └── builder/
│       ├── builder.hpp          # query_gateway / query<T> / update_builder / remove_builder
│       └── expression.hpp       # member_key / predicate / 谓词构造器
├── src/                         # 对应实现（编译进 libuniorm）；私有头贴着 .cpp 存放
│   ├── orm_mapping.hpp          # 实体写的 WHERE 解析与 SET/WHERE 列划分（纯映射规则）
│   ├── statement_cache.hpp      # LRU 预编译语句缓存（存 statement_iface，容量固定 64）
│   ├── backend/                 # scheme 解析与注册表实现
│   └── odbc/                    # ODBC backend（自注册 "odbc"）
│       ├── backend.hpp          # backend 契约的 ODBC 实现（含 odbc_batch_writer）
│       ├── environment.hpp / connection.hpp / statement.hpp   # 句柄 RAII
│       ├── handles.hpp          # 句柄 RAII 模板、traits
│       ├── native_types.hpp     # SQL_* → sql_type 映射表（驱动编码只活在这里）
│       └── error.hpp            # odbc_error / diagnostics
├── tools/uniorm-gen/            # 代码生成：uniorm_gen_core(STATIC) + uniorm-gen(CLI)
│   ├── main.cpp                 # 参数解析与编排（唯一进 CLI 的源文件）
│   ├── schema_reader.cpp/.hpp   # ODBC 元数据提取（直连私有句柄层，顺手归一 DATA_TYPE）
│   ├── generator.cpp/.hpp       # model + 配置 → 头文件文本
│   ├── config.cpp/.hpp          # TOML 子集解析
│   ├── naming.cpp/.hpp          # PascalCase/camelCase 标识符转换
│   └── schema_model.hpp         # 中间 schema 模型（生成器输入；列类型已是中立 sql_type）
├── tests/
│   ├── unit/                    # 无库依赖：check.hpp（CHECK/CHECK_THROWS）+
│   │                            # uniorm_unit_tests（不链 ODBC，含 fake backend 的
│   │                            # test_pool.cpp）与
│   │                            # uniorm_odbc_unit_tests（句柄 RAII、错误派生、uniorm-gen）
│   ├── integration/             # 需活连接：test_integration.cpp、test_gen_e2e.cpp
│   │                            # 与 golden/gen_it_schema.hpp（生成物快照）
│   ├── install/                 # 外部消费者工程（install_smoke 用它走 find_package）
│   ├── install_smoke.cmake.in   # 安装面冒烟脚本（cmake -P）
│   └── perf/                    # test_perf.cpp（ctest 标签 perf）
├── docs/design.md
├── README.md / README.zh.md     # 双语入口文档
├── .clang-format                # Google 风格基线（未挂 hook，流水线也不校验格式）
├── .github/workflows/ci.yml     # 两条形状：无 ODBC 的编译契约 + 双驱动对活库跑 ctest
└── .gitignore
```

公开/私有边界按"外部消费者是否需要"判定：凡出现在 `uniorm/uniorm.hpp` 或
`uniorm/mapping/registry.hpp`（生成代码的唯一依赖）传递闭包内的头文件留在 `include/`，
其余下沉到 `src/`，与自己的实现 `.cpp` 贴邻，用引号相对名互相引用。于是 `src/`
不在 `uniorm` 目标的任何 include 路径上（同目录引用无需路径），只有确实需要跨目录取用
私有头的四个目标显式 `-I src`（PRIVATE）：`uniorm_gen_core` 与 `uniorm-gen`
（直调 `SQLTables` / `SQLColumns` 等目录函数）以及两个白盒单测。公开头一旦
`#include` 私有头便无法解析，边界由编译器强制；库内的 `<sql.h>` 只出现在
`src/odbc/` 之下，对外头文件既不带驱动类型，也不带 ODBC 链接依赖
（`ODBC::ODBC` 是 PRIVATE）。

公开头只留声明：非模板成员的定义一律进同名 `.cpp`（`orm.cpp`、`decimal.cpp` 都按
这条走）。定义搬出类外时导出标记不会跟着走——类外的 `operator` 友元要在声明上
自己写一次 `UNIORM_API`，`converter` 的显式特化则把标记写在整个特化上。

### 3.1 安装与集成

`cmake --install build --prefix <p>` 产出四类文件（目录名取自 `GNUInstallDirs`，
64 位 RHEL/Fedora 上 `<libdir>` 解析为 `lib64`）：

| 位置 | 内容 |
|---|---|
| `<libdir>/` | `libuniorm.so.<VERSION>` 加 `SOVERSION`（`0.2`）与裸名两级符号链接；Windows 下 DLL 走 RUNTIME、导入库走 ARCHIVE |
| `include/uniorm/` | 全部 public 头文件；私有头贴邻 `.cpp` 留在 `src/`，不参与安装 |
| `<libdir>/cmake/uniorm/` | `uniormConfig.cmake`、`uniormConfigVersion.cmake`、`uniormTargets.cmake` 与 `uniormTargets-<config>.cmake` |
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
- ODBC 与线程都是 PRIVATE 依赖，不进导出接口：`libodbc.so` 由 `libuniorm.so` 自己的
  `DT_NEEDED` 载入，消费者无需 `find_dependency(ODBC)`。
- `uniorm-gen` 走 RUNTIME 安装但不进 `EXPORT`：它是"跑一遍"的程序，不是被链接的
  目标，导出它便等于把 `uniorm_gen_core`（内部静态切分，靠 `-I src` 读私有头）
  伪装成对外 API。它的 `DT_NEEDED` 写死 `libuniorm.so.<SOVERSION>`（现为 `0.2`），
  而构建树留下的 `RPATH` 是绝对路径，故 ELF 上以 `INSTALL_RPATH` 改写成
  `$ORIGIN/../<libdir>`——装到哪个 prefix 就找哪个 prefix，与库同树发布时版本必然对上。

导出的 CMake 文件不含绝对前缀：`DESTDIR=<stage> cmake --install build --prefix
/usr/local` 能打到暂存根再整体搬迁（实测装出来的 4 个 CMake 文件里既无 stage 也
无 prefix 的字面量）。暂存根下的 `uniorm-gen` 也能直接跑起来——`$ORIGIN` 相对路径
要买的就是这一点。

上面这些如今由 `install_smoke`（§8）把关：装进构建树下的临时 prefix，比对装出来的
头文件集合与 `include/` 一致，用一个刻意不设 `CMAKE_CXX_STANDARD` 的外部工程
`find_package` + 编译 + 运行，再故意以 `99.0.0` 配置一次要求被拒，最后跑装出来的
`uniorm-gen --help` 证明 `$ORIGIN/../<libdir>` 真的载到了同 prefix 的库。

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
    void open_dsn(std::string_view dsn, std::string_view user, std::string_view password);
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
  无游标更新）、`SQL_ATTR_ASYNC_ENABLE`（无异步）。

### 4.2 Unicode 策略

- 库内部所有 `std::string` / `string_view` 均为 UTF-8；
- ODBC 边界一律窄字符：列与参数按 `SQL_C_CHAR` / `SQL_VARCHAR` 绑定，连接串按
  `SQLDriverConnect` / `SQLConnect` 的窄接口提交，库内不做任何编码转换；
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
`src/odbc/native_types.hpp`，核心库的任何一处都不再出现驱动编码）；
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
| TINYINT / SMALLINT / INTEGER / BIGINT | `int16_t` / `int32_t` / `int64_t`（TINYINT 也接受 `int8_t`） |
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

// 高层连接：按连接串 scheme 选择 backend，move-only
class connection {
    explicit connection(std::string_view connection_string);
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

    // 逃生舱：调用方点名期望类型（ODBC 下 T = void 的 SQLHDBC；backend 扩展
    // 如 backend::schema_metadata），不支持时返回 nullptr
    template <class T> T* native_handle() noexcept;
    template <class T> T* extension() noexcept;

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
- 因此 `column_meta` 除 `write`（populate）/`read`（行式值提取）/`make_binding`
  （查询直绑）外，还带列式通道要用的 `write_to_param_buffer` 与 `get_string_size`

### 4.6 聚合 struct 投影（零注册路径）

无需任何映射注册，按列序绑定聚合体：

```cpp
struct user_row {
    int64_t id;
    std::string name;
    std::optional<int32_t> age;
};

// orm 上的模板重载（connection 没有 query 成员）；实体查询走 db.query().of<T>()
auto rows = db.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", {18});   // std::vector<user_row>
for (auto const& u : rows) { /* ... */ }
```

返回前按 `orm::row_array_size()` 块取行，逐行 `fill_into` 直接构造进结果 vector
的存储（见 §4.8 物化路径）。

实现：自实现 `pfr_lite`（`detail/pfr.hpp`，不引入 Boost），含两个核心设施——
`field_count<T>`（万能转换类型 + `requires` 表达式探测聚合体字段数）与
`tie_aggregate(T&)`（按字段数 `if constexpr` 分派的结构化绑定展开，由宏生成至上限）。
要求：入口 concept `detail::aggregate_projection` = `std::is_aggregate_v` +
`std::is_default_constructible_v` + `field_count<T>() <= max_aggregate_fields`（64）；
concept 挂在 `orm::query<T>` 的模板参数上，不满足者是"无匹配重载"而非体内
`static_assert`。字段类型满足 4.3 的映射或存在 converter；嵌套聚合体计为一个字段，
需经 converter 绑定。

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

struct entity_meta {
    std::string table;
    std::vector<column_meta> columns;
    std::vector<member_key> ignored;

    std::string const& column_name(member_key const& key) const;  // 未注册抛 mapping_error
    void populate(void* obj, row const& r) const;                 // 按列名写回对象
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
    explicit orm(connection_pool& pool);                // 用户自管池
    void connect(std::string_view connection_string);
    void disconnect();

    // 同名 getter/setter，默认值见括号
    std::size_t row_array_size() const noexcept;  void row_array_size(std::size_t);  // default_row_array_size = 100
    std::size_t paramset_size() const noexcept;   void paramset_size(std::size_t);   // default_paramset_size = 1000
    bool auto_commit() const noexcept;            void auto_commit(bool);            // true：即连接的 autocommit 属性（见 §4.9）

    template <class T> mapping_builder<T> map(std::string_view table);  // 重复注册抛 mapping_error
    template <class T> entity_meta const& meta() const;                 // 未注册抛 mapping_error
    entity_meta const* find(std::type_index type) const;
    std::size_t size() const noexcept;

    void validate(validation_mode mode = validation_mode::strict);
    // 不接 connection 参数：经 native_connection().extension<backend::schema_metadata>()
    // 取表元数据，backend 不提供该扩展则抛 mapping_error。逐实体核对：
    //  - 表不存在（元数据为空）      → mapping_error
    //  - 列缺失                       → mapping_error
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
    query_gateway query();                                  // 实体查询：db.query().of<T>()
    template <detail::aggregate_projection T>
    std::vector<T> query(std::string_view sql, params const& p = {});    // 投影
    update_builder update(std::string_view table);          // 无映射表的动态 UPDATE
    remove_builder remove(std::string_view table);          // 动态 DELETE

    result_set execute(std::string_view sql, params const& p = {});      // 转发 row_array_size()
    std::size_t execute_update(std::string_view sql, params const& p = {});
    transaction begin();  void commit();  void rollback();

    unsigned long long statement_cache_hits() const;    // 转发底层 connection
    unsigned long long statement_cache_misses() const;
    std::size_t statement_cache_size() const;
    void clear_statement_cache();
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
`auto_commit`）、`find` / `size` 与 `native_connection` 都定义在 `src/orm.cpp`，
头文件里只剩下声明、两个 `default_*` 常量，以及必须由调用方实例化的模板。写路径
因此并未变慢：批量入口读的是 `paramset_size_` 成员本身，是否自开事务看
`connection::autocommit()`（一个缓存的提交模式），每行一次的循环里没有新增
任何跨库调用。

成员指针的类型擦除：注册时经 `make_column_meta` 捕获 `write` 闭包
（`[member](void* obj, sql_value const& v) { static_cast<T*>(obj)->*member = value_cast<M>(v); }`），
运行时物化不再需要模板。查询结果按**列名**（而非列序）写回，
天然规避 SELECT 列序与 struct 字段序不一致的问题。

### 4.8 成员指针查询构建器

```cpp
auto users = db.query()
    .of<User>()
    .where(gt(&User::age, 18) && eq(&User::status, Status::Active))
    .order_by(&User::name, direction::desc)
    .limit(50)
    .all();                        // std::vector<User>

auto count = db.query().of<User>()
    .where(col(&User::age) > 18)
    .count();

auto opt = db.query().of<User>()
    .where(col(&User::id) == 42)
    .one();                        // std::optional<User>
```

谓词写法说明：不使用 `&User::id == 42` 这类直接对成员指针重载运算符的形式。
原因有二：C++20 下模板化 `operator==` 会与"翻转候选"重写规则冲突产生歧义；
且 GCC 11 对成员指针操作数不做 ADL，运算符在多数编译器上根本找不到。
因此提供命名函数 `eq/ne/lt/le/gt/ge`，以及 `col(&User::x)` 包装后的中缀写法；
`&&` / `||` / `in` / `is_null` / `like` 作用于 `predicate` 本身，不受影响。

入口在 `orm` 上而非 `connection` 上（`connection` 没有 `query` 成员）：
`db.query()` 返回轻量网关 `query_gateway`，只持一个 `orm*`，`.of<T>()` 产出
`query<T>` 构建器；连接经 `orm::native_connection()` 取得，事务语义因此跟随
`orm` 当前持有的连接（`orm` 内部持 `std::optional<pooled_connection>`，见 §4.10）。
构建器只存网关的指针，网关是表达式里的临时对象——链式调用没问题，不要把
`of<T>()` 的结果跨语句存下来。

物化路径：`all()`/`one()` 不经过 `result_set`/`row`/`sql_value`，而是把结果列
**一次 `bind_column` 到暂存缓冲（ODBC 下即一次 `SQLBindCol`），再按字段偏移直写
目标实体**。注册时 `column_meta` 除 `write`/`read` 闭包外再生成 `make_binding`
工厂（`detail::make_field_binding` 按成员类型选择数值直绑、字符串/二进制定长
缓冲 + 截断重读、时间戳暂存、`std::optional` 空值复位等绑定策略）。变长列的
暂存槽宽取各绑定期 `stmt.column_meta()` 的 `display_size`：字符列
`display_size × 4 + 1`（`display_size < 1024` 时），二进制列 `display_size + 1`
（`< 4096` 时），否则退回 4096 定宽——使一个块的缓存占用与列宽成比例；超槽或
驱动回报 `SQL_NO_TOTAL` 的值退回 `read_long_text`/`read_long_bytes` 整读。
`render_select` 保证 SELECT 列序与 `meta.columns` 注册序一致；每个绑定在构造期
捕获字段在实体内的**字节偏移**，`finalize(row_index, entity)` 把某行的暂存数据
直接写入任意实体实例。`all()` 因此可以按块 `resize` 出 vector 存储后逐行
`fill_into(out[i])`（异常时把 size 截回原值再重抛），无原型对象中转、每字段一次
赋值即完成物化；`one()` 仍经 `take()` + `proto_` 返回。

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

- 列名解析：成员指针 → 经 `entity_meta::column_name` 查注册表（未注册成员抛 `mapping_error`）；
- 生成 SQL 使用 `?` 占位符（ODBC 原生参数标记），值按序收集进 `params`；
- 标识符引用与分页语法经 `dialect` 生成：

```cpp
struct dialect {
    char quote_open = '"', quote_close = '"';     // MySQL/MariaDB → ` `
    bool ansi_pagination = true;                  // false → LIMIT/OFFSET

    std::string quote_identifier(std::string_view identifier) const;
    // ANSI: " OFFSET n ROWS FETCH NEXT m ROWS ONLY"；否则 " LIMIT m OFFSET n"
    std::string pagination(std::optional<std::size_t> limit, std::size_t offset) const;

    static dialect detect(std::string_view dbms_name);   // 输入 connection::dbms_name()
};
```

构建器与网关最终 API：

```cpp
enum class direction { asc, desc };

template <class T>
class query {                                  // 只能由 query_gateway::of<T>() 产出
public:
    query& where(predicate p);                       // 多个 where 以 AND 连接
    template <class M> query& order_by(M T::*member, direction dir = direction::asc);
    query& limit(std::size_t n);
    query& offset(std::size_t n);
    std::string build_select() const;                // 干跑生成 SQL，不执行
    std::vector<T> all();
    std::optional<T> one();                          // 渲染时强制 limit 1
    std::int64_t count();                            // SELECT COUNT(*)，走 result_set
    template <class M, class V> query& set(M T::*member, V&& value);  // 暂存赋值，nullptr 写 NULL
    std::size_t update();                            // 无 set() 或无 where() 抛 uniorm_error
    std::size_t remove();                            // 无 where() 抛；两者忽略 order_by/limit/offset
};

class query_gateway {                                // orm::query() 的返回值，只持一个 orm*
public:
    template <class T> query<T> of();                // 未注册实体抛 mapping_error
    connection& conn() const;                        // 转发 orm::native_connection()
    orm& get_orm() const;
    std::size_t row_array_size() const noexcept;     // 转发 orm::row_array_size()，即块取行大小
    dialect const& sql_dialect() const;              // 首次调用时探测并缓存
};

// 无实体映射的表上的动态 UPDATE / DELETE：orm::update(table) / orm::remove(table)
class update_builder {
public:
    template <class V> update_builder& set(std::string_view column, V&& value);
    update_builder& where(std::string_view clause, params p = {});
    std::size_t execute();                           // set 空或 where 空白即抛，不放全表火
};

class remove_builder {
public:
    remove_builder& where(std::string_view clause, params p = {});
    std::size_t execute();                           // where 空白即抛
};
```

`update_builder` / `remove_builder` 的 `where(clause, p)` 与 `query<T>::where(predicate)`
是两套东西：前者把 `clause` **原样拼进 SQL**（表名与列名经 `dialect::quote_identifier`，
WHERE 片段本身不解析也不引用，值仍以 `?` 绑定，参数排在 SET 值之后）。

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
    std::string connection_string;
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

// 全局池注册表：orm 用连接串构造时即从这里借连接（自动池化）
class connection_pool_registry {
public:
    static connection_pool_registry& instance();
    pooled_connection acquire(std::string const& connection_string);
    void configure(std::string const& connection_string, pool_options opts);
};
```

`orm` 的三条连接路径：`orm(connection_string)` / `orm::connect(...)` 走注册表
（key 为解析出的 `dsn=` + `uid=`（含 `user=`/`username=` 别名），大小写与空白
归一后取 `dsn=<值>|uid=<值>`；首次见到的那串完整连接字符串成为该池的建连串），
缺省参数即上面的 `pool_options` 默认值；`orm(connection_pool&)` 用调用方管
的池；`orm()` + `map()` 只做注册，首次使用时 `ensure_connected()` 若仍未连接
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

## 5. Backend 抽象：多后端规划与 v1 纪律

背景：libpq 与 Oracle OCI 在实际项目中都存在必须独立使用原生 API 的场景（PG：COPY、LISTEN/NOTIFY、异步 I/O；Oracle：OCI 数组绑定、高级特性等），这些是 ODBC 无法覆盖的。因此 uniorm 采用"通用层打底 + 原生特性逃生舱口"策略，而非试图把各库特性塞进统一 API。

### 5.1 边界纪律

backend 接口已在里程碑 1 落地（§5.2），下列纪律从约定变成构建结构：

- ODBC 类型（`SQLH*`、`SQLLEN`、indicator、SQLSTATE）不得出现在语句层及以上层级的
  公共 API 中：`include/` 下没有任何文件包含 `sql.h` 或写出 `SQL*` 类型，驱动类型
  只活在 `src/odbc/`；
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

**这条保证只到链接行为止**：ODBC 是 `PRIVATE` 链接，驱动符号不进消费者的链接行，
但默认构建下 `ldd libuniorm.so` 仍列出 `libodbc.so.2`——进程载入本库时驱动管理器
照样被映射。想运行时也不碰 ODBC，眼下 `-DUNIORM_BACKEND_ODBC=OFF` 就够了
（实测该构建的 `ldd` 无 odbc 项），因为 ODBC 是唯一内置 backend；等 libpq/OCI
进来，单库会把各家驱动的依赖一并带上，那时才需要按 backend 拆目标（见 §9）。

`UNIORM_BACKEND_ODBC=OFF` 时 `src/odbc/*` 整体不参与编译（此时库内没有任何
backend 实现，连接一律抛 `unknown_scheme`），`uniorm_odbc_unit_tests`、集成测试、
perf、`uniorm-gen` 都不生成，`UNIORM_BUILD_TOOLS` 直接被 CMake 拦下；
`uniorm_unit_tests` 则按选项定义 `UNIORM_TEST_BACKEND_ODBC`，依赖 ODBC 注册的
用例（`reg.contains("odbc")`）在该宏内，OFF 时自动不参与断言。

### 5.2 backend 接口（v2 里程碑 1，已实现）

接口位于 `include/uniorm/backend/backend.hpp`，ODBC 是唯一内置实现
（`src/odbc/backend.cpp`）。核心 API（查询构建器、映射、池、事务）只依赖
接口；能力缺失时应当抛清晰错误而非静默降级——这条纪律目前只在 `columnar_batch`
一个能力上真正生效（见下方能力清单）。

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

**语句与连接接口**：

```cpp
struct capabilities { bool streaming, async_io, copy_protocol,
                             notifications, columnar_batch; };

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
    std::string read_long_text(std::size_t column);
    std::vector<std::byte> read_long_bytes(std::size_t column);
    void reset();
};

struct batch_writer_iface { std::size_t add_column(buffer_type, count, element_size);
                            void* data(col); std::size_t element_size(col);
                            std::int64_t* indicators(col); void finish(); };

struct connection_iface { /* open / close / is_open / set_autocommit /
                            commit / rollback / caps / dbms_name /
                            create_statement / native_handle /
                            extension(std::type_index) */ };

struct schema_metadata { /* table_columns(table) → {name, type, native_type,
                            nullable}；供 orm::validate；经 extension() 查找 */ };
```

ODBC 实现的当前能力：`{streaming=true, async_io=false, copy_protocol=false,
notifications=false, columnar_batch=true}`。**只有 `columnar_batch` 被读**
（`orm` 的三个批量入口据此在列式与行式通道间二选一，见 §4.5.2）。每个标志只表示
"有一条更快的路"：缺能力时核心走慢的那条而不抛错，所以 backend 全置 `false`
也不失正确性。`capability_not_supported` 已定义、无抛出点，留给将来确实无路可退的
核心特性（§9 第 9 项）。`streaming` / `async_io` / `copy_protocol` /
`notifications` 四个标志位是为 libpq/OCI 预留的占位。

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
- 注册表为 Meyers 单例 + mutex；重复注册抛 `backend_error`，未注册 scheme
  抛 `unknown_scheme` 并列出已注册项；
- ODBC backend 以文件作用域 `static registrar` 自注册（编进
  `libuniorm.so`，加载即达）。

**错误体系**：`backend_error : uniorm_error`（backend 名 + context +
`diagnostic{state, native_code, message}`）；`odbc_error` 为其派生
（backend 名固定 "odbc"），故现有 catch 站点不受影响；另有
`capability_not_supported` 与 `unknown_scheme`。

**构建门禁**：`option(UNIORM_BACKEND_ODBC ON)`；ODBC 由 PUBLIC 收紧为
PRIVATE 链接；`uniorm-gen` 直接读 ODBC 元数据，故 `UNIORM_BUILD_TOOLS`
依赖该选项。

### 5.3 原生特性通道

**原生句柄逃生舱口**——保证"ODBC 做不到的事永远有路可走"：

```cpp
auto* pg = conn.native_handle<PGconn>();   // 调用方命名期望的原生句柄类型
PQputCopyData(pg, ...);                     // 用户自行驱动原生操作
```

连接与事务生命周期仍由 uniorm 管理；原生操作发生在借出的连接上，归还前状态必须自洽。

**类型化扩展接口**——对高频原生特性提供半官方封装，不可移植性由用户在调用点显式选择：

```cpp
if (auto* ext = conn.extension<postgres_ext>()) {
    ext->listen("order_events", callback);
    ext->copy_in("orders", row_source);
}
if (auto* ext = conn.extension<oracle_ext>()) {
    ext->bulk_insert("orders", rows);              // OCI 数组绑定
}
```

### 5.4 已确认的 backend 优先级

1. **libpq**（PostgreSQL）——COPY、LISTEN/NOTIFY、异步 I/O
2. **Oracle OCI**——数组绑定及 OCI 专有特性

两者均来自既有项目中必须绕开 ODBC 的实际经验。扩展接口与能力清单按上述特性集设计。

## 6. uniorm-gen 代码生成工具

### 6.1 形态

独立 CLI，活连接目标数据库。本仓库只提供可执行文件（`UNIORM_BUILD_TOOLS=ON`
时构建，因它直读 ODBC 元数据而依赖 `UNIORM_BACKEND_ODBC`；顶层构建时也按 §3.1
装进 `<bindir>`）；仓库内没有任何 `add_custom_command`，"构建期生成"要调用方自己
在 CMake 里接。

```
uniorm-gen (--dsn=<dsn> [--user=<u> --password=<p>]
           | --connection-string=<str>)
           --out=<dir>
           [--config=<file>]          # TOML，见 §6.4
           [--tables=a,b,c]           # 逗号分隔的表名过滤
           [--catalog=<c>] [--schema=<s>]
           [--name=<n>]               # 产物/命名空间名，缺省取 SQL_DATABASE_NAME
```

`--dsn` 与 `--connection-string` 必须二选一，`--out` 必填，否则打印 usage 并
返回失败。

### 6.2 Schema 提取（纯 ODBC 元数据）

- `SQLTables` → 表清单（只按 `--catalog` / `--schema` 过滤，表名传 `NULL`；
  结果里只留 `TABLE` / `BASE TABLE`）
- `--tables` 是**客户端筛选**：先精确名匹配，全库无同名时再退一次大小写不敏感
  匹配（`lower_case_table_names` 的服务器），两边都对不上就告警并跳过该项
- `SQLColumns` → 列名、ODBC `data_type`、驱动类型名、`column_size`、
  `decimals`、可空、默认值
- `SQLPrimaryKeys` → 主键（生成物里标 `.primary_key()`）
- `SQLForeignKeys` → 外键（v1 仅记录，不生成关联导航）
- `SQLStatistics` → 索引

外键与索引都只以注释形式进生成物（`// FK: col -> pk(col)`、`// index: name (cols)`），
无开关；`uniorm-gen` 唯一读的 `SQLGetInfo` 是 `SQL_DATABASE_NAME`（用作缺省单元名）。

### 6.3 生成物

每库一个头文件 `<out>/<name>_schema.hpp`，整体包在 `namespace <name>`（经
`to_unit_name` 归一为 `[a-z0-9_]`）里，内容为：

1. 每表一个 `struct`：表名 → PascalCase 类名、列名 → camelCase 成员名，
   C++ 关键字/非法起始字符会被改写。**命名规则固定，不可配置**——可配置的
   只有 §6.4 里逐表的 `class` 覆写；
2. 可空列 → `std::optional<T>`；
3. 每表一个 `inline void register_<Class>_mapping(orm&)`，外加汇总的
   `inline void register_<name>_schema(orm&)`；
4. SQL 类型 ↔ C++ 类型映射遵循 §4.3 表，可被配置覆写。

生成物只输出到指定目录，视为不可手改。头文件仅依赖
`<uniorm/mapping/registry.hpp>` 与标准库。

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
"uniorm::decimal_t"` 用不着消费者额外 include：生成物唯一依赖的
`<uniorm/mapping/registry.hpp>` 传递带着 `<uniorm/decimal.hpp>`。

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
└── odbc::odbc_error                     // ODBC 句柄层（src/odbc/error.hpp，私有头），
                                         // backend 名固定 "odbc"

backend::capability_not_supported : uniorm_error   // 能力缺失；备用类型，无抛出点，
                                                   // 见 §5.2
backend::unknown_scheme : uniorm_error             // 连接串 scheme 未注册

gen::config_error : uniorm_error                   // uniorm-gen 的 TOML/类型配置错误
                                                   // （tools/uniorm-gen/config.hpp）
```

所有 ODBC 失败都经 `throw_if_error` 这一处收口抛 `odbc_error`，而它就是
`backend_error`：公开侧按 `uniorm::backend::backend_error` 捕获即可拿
`backend_name()` 与 SQLSTATE 诊断记录，不需要私有头，适配器也不再需要二次翻译。
集成测试 `test_error_reporting` 钉住这条跨层契约。

## 8. 测试策略

除末条安装冒烟是 `cmake -P` 脚本外，下列用例都是编译进 CTest 的 C++ 程序。

- **单元测试**（无数据库，已实现，拆为两个目标）：
  `uniorm_unit_tests` 不链接 ODBC——`test_pfr`（字段数探测/展开/concept 负例）、`test_row`
  （value_cast/收窄/optional/文本字面量按需解析成算术目标）、
  `test_params`（值归一化）、`test_converter`
  （converter 优先于枚举与隐式转字符串两支、表示决定读侧绑定与批量暂存、列的可接受
  类型族、按值取槽位的 `from_db` 搬走暂存缓冲区且跨行复用绑定后仍成立）、
  `test_decimal`（字面量解析与 `to_literal` 往返、小数位数不被规格化掉、整数前导零、
  负零、指数/分隔符/多小数点/超 78 位的拒绝、跨 scale 的序与等值、
  `to_double` / `to_int64` 及两者越界、动态行经 `value_cast<decimal_t>` 与
  `converter<decimal_t>` 的读写往返）、
  `test_expression`（谓词 SQL 生成、方言、分页）、`test_registry`
  （映射注册/populate/read 闭包/错误路径）、`test_orm_crud_helpers`
  （实体 CRUD 的映射级归一：全部键列推断、WHERE 字段解析与 SET 分区、
  `paramset_size` / `row_array_size` 的 0 归一）、`test_backend_registry`
  （scheme 解析边界、注册/重复注册/未注册 scheme；其中真正解析到
  "odbc" backend 的用例在 `UNIORM_TEST_BACKEND_ODBC` 宏内）、`test_pool`
  （用一个记录调用的假 backend 驱动 `connection_pool::release`：归还时回滚挂起的
  工作并复位 autocommit，复位抛异常则该连接被淘汰且名额扣回）；
  `uniorm_odbc_unit_tests` 链接 ODBC——`test_odbc_handles`（句柄 RAII）、
  `test_odbc_error_is_backend_error`（`odbc_error` 就是 `backend_error`，
  无需二次翻译）、`test_gen_config`（TOML 子集解析正例/错误行号/非法键）、
  `test_gen_output`（命名转换边界 + 生成器快照与覆写/跳表/converter 生成/DECIMAL 列
  以 `cpp_type` 覆写成 `uniorm::decimal_t`/错误路径）；
  后两个只在 `UNIORM_BUILD_TOOLS` 打开时编入（同时定义 `UNIORM_TEST_GEN`），
  因为它们要链 `uniorm_gen_core`；
- **集成测试**（已实现，DSN/凭据由 `UNIORM_IT_DSN` / `UNIORM_IT_USER` / `UNIORM_IT_PWD` 指定，凭据以 `UID`/`PWD` 写进连接串；连不上时 ctest SKIP）：execute/params 往返、动态行（含 `connection::execute` 显式块取行大小 0 退回逐行）、DECIMAL 动态路径（`DECIMAL(20,4)` / `(20,0)` / `(38,0)` 取回精确定点字面量与 `column_info::scale`，按需解析成 double、scale-0 解析成 int64，带小数或超 int64 范围的字面量抛 `type_mismatch`，NULL 仍报 `is_null`）、DECIMAL 映射路径（`DECIMAL(20,4)` / `(65,30)` / `(20,0)` 三列配 `decimal_t` 与 `std::optional<decimal_t>` 成员：批量插入与实体读回逐字核对 `to_literal`（含 65,30 列第 30 位为 1 的值——没有任何整型或 double 装得下它）、聚合投影、动态行 `get<decimal_t>`、以 `decimal_t` 为右值的 `where(gt(&T::amount, ...))` 落到同一字面量）、聚合投影（含长字符串与 timestamp）、converter 往返（批量插入、实体物化含 NULL、
`in` 谓词、投影、构建器 `set`/批量 update、动态行 `get<T>`）、orm validate（含 strict
的列缺失/可空/类型族三条失败路径）、查询构建器全谓词与分页、事务 commit/rollback/析构回滚、批量插入（含空 optional 写 NULL、1500 行跨 `paramset_size` 分批）、批量 update / 批量 remove（实体版按主键与全字段两种 WHERE，含一张复合主键表验证单实体与批量都按全部键列命中、非键行不被牵连，动态版 `orm::update(table)` / `orm::remove(table)`，以及 `query<T>::set/update/remove` 与无 WHERE / 无 SET / WHERE 字段未映射的守卫抛错）、语句缓存（hit/miss 计数、流式 result_set 借出期间并发 miss、清空）、跨层错误上报（驱动失败以 `backend_error` 捕获，核对 `backend_name()`
与 SQLSTATE 诊断）、连接池借还与超时、连接池维护（心跳保活计数、空闲超时驱逐、失败心跳丢弃；"排空"一律轮询等待而非单次采样，因为正被心跳的连接仍计入 `idle_count()`）；后续按库加条件标签覆盖方言与类型怪癖；
- **性能基准**（已实现，ctest 标签 `perf`，`tests/perf/test_perf.cpp`）：
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
  另含**纯 ODBC 基线**（不经 uniorm，直接操作句柄，只保留与 uniorm
  同名同形的用例）：单行 `VALUES (?, ?, ?, ?)` + `SQL_ATTR_PARAMSET_SIZE` +
  列方向量数组 + 逐值 `SQLBindParameter` 的批量插入/更新/删除（与 uniorm 的
  数组绑定同构，不存在多行 VALUES 或占位符上限分批）、`SQLBindCol` + `SQLFetch`
  全表扫描（对应实体直绑与动态行路径）、单行 `LIMIT 1`（对应 `one()`），
  用于衡量 uniorm 抽象层的额外开销
- **`uniorm-gen` 端到端**（已实现，`gen_e2e_tests`，连不上库时 SKIP）：
  夹具表（含 PK/FK/索引/DECIMAL/DATETIME）→ 工具带检入的覆写文件
  （`golden/gen_it.toml`，其 `converter` 键让 `note` 生成为域类型，`cpp_type` 键把
  `amount` 生成为 `uniorm::decimal_t`）生成 →
  与 golden 比对，但只比代码：两边每行 `//` 之后的注释先截掉再比，不分哪一格。
  注释记的是连接器怎么拼 `BIGINT`、服务端怎么存默认值，没有消费者会编译它，而它
  按"连接器 × 服务端"每格都不同——留着它就等于把 golden 钉死在一格上，升级一支
  连接器能同时红四条腿。截注释不动行结构（成员行是固定两个空格的分隔符），所以
  两边仍要对齐每一行的起点。另留一组**语义标记**（FK 与二级索引的注释）：这两件
  事在生成的代码里不留任何痕迹，只有注释能证明抽取到了。主键映射调用、可空列与
  `decimal_t`/`timestamp` 生成的 C++ 类型都是代码，归代码比对看着。之所以代码
  这一层要每条腿都跑，是因为服务端答空一处元数据时生成的头文件照样编译、注册、
  过 `validate(strict)`，MySQL 8.4 上就真发生过主键整列读丢（见 §8）——它在代码
  比对里就是 `.column` 撞上 golden 的 `.primary_key`。
  golden 本身被编译进测试，执行注册 +
  `validate(strict)` + 构建器 `count()` + 实体写入与物化读回（读回同时核对
  `decimal_t` 的值相等与按列 scale 还原出的字面量），覆盖"生成 → 编译 →
  注册 → 校验 → 读写"全链路。
- **安装冒烟**（已实现，`install_smoke`，`cmake -P` 脚本，只在 top-level 且非交叉编译
  时注册，不需要数据库）：`cmake --install` 到 `<build>/install_smoke/prefix` → 核对
  config/targets/头文件/库都已就位，且装出的头文件集合与 `include/uniorm` 逐一相符 →
  配置并构建 `tests/install/` 这个外部工程（刻意不设 `CMAKE_CXX_STANDARD`，靠导出目标
  携带），跑起来的消费者调用 .so 里的 `dialect::detect`、`parse_scheme`、未注册 scheme
  抛 `unknown_scheme`，ODBC 构建下另核对 `"odbc"` 已随载入自注册 → 再以 `99.0.0` 配置
  一次，要求被 `SameMinorVersion` 拒掉 → 最后运行装出来的 `uniorm-gen --help`，它只可能
  经 `$ORIGIN/../<libdir>` 载到库，故 RPATH 改写一并验了。
- **CI**（已写入 `.github/workflows/ci.yml`；GitHub 上跑绿过的那副形状是三支作业——
  `core` 与两条驱动腿，三支都按作业原样在 ubuntu:24.04 容器里重放过，服务端用的是一只照抄作业
  `services` 块起出的 `mariadb:11`（实测 11.8.9），连 `MARIADB_DATABASE`/`MARIADB_USER`
  生成的授权与 `mariadb-admin ping` 健康门（约 20 s 转 healthy）也一并验了；提交过
  三趟，头一趟卡在 YAML 校验，第二趟作业真跑起来了、`core` 当场抓出一处真漏，第三趟三支作业
  全绿——前两样的账都在下面。此后驱动那一支沿服务端铺成 2×2，新形状还没被 runner 看过）：
  一支 `UNIORM_BACKEND_ODBC=OFF`
  的构建只跑 `unit_tests`，替 §3 那条"驱动类型不漏进 statement 层之上的公开头"把关——
  这条承诺此前只在注释里，没有任何东西在守它。另一支按**连接器 × 服务端**成 2×2 矩阵，
  每格对自家的服务容器跑除 `perf` 外的全部五条：MySQL Connector/ODBC 取自 MySQL 自己的
  apt 组件（Ubuntu 归档里没有它），MariaDB Connector/ODBC 只能从 tag 拉源码构建（Ubuntu
  任何发行版都不打包它，上游 release 也不带二进制）；服务端两格是 `mysql:8.4`（实测
  8.4.11）与 `mariadb:11`（实测 11.8.9）。服务端这一轴不是因为 SQL 会长得不一样——
  `dialect::detect` 对两个 banner 给同一套引号与分页——而是因为 `uniorm-gen` 读的是
  **服务端答的元数据**：本地拿真 MySQL 服务端跑这套测试，第一趟就撞出 MariaDB 连接器
  `3.1.12` 用 `COLUMN_KEY = 'pri'` 问 `information_schema`，而 8.4 把那些列声明成
  `utf8mb3_bin`（区分大小写，实际值是 `PRI`），于是 `SQLPrimaryKeys` 空返回、生成的
  头文件把两张表的主键整列读丢且不报错。当时把它暴露出来的只有 golden 的字节差，
  而跳过 golden 的腿看不见它。同一夹具换 `3.1.23` 无恙，因为它问的是
  `KEY_COLUMN_USAGE` 的 `CONSTRAINT_NAME = 'PRIMARY'`。CI 钉的是源码构建的
  `3.1.23`，复现不了旧连接器，所以这类沉默改由比对本身兜：`gen_e2e` 四条腿都比
  截掉注释之后的代码（见 §5），那一格的读丢就是 `.column` 撞上 golden 的
  `.primary_key`。注释从此一处不比，因为它们在四格里本就没有一样的时候：两条
  MySQL 服务端腿连 `DEFAULT NULL` 都拿不到，两条 Connector/ODBC 腿把类型名拼成
  小写。两只镜像各带自家的健康检查客户端，`mysql:8.4` 只有
  `mysqladmin`、`mariadb:11` 只有 `mariadb-admin`，故健康命令按 matrix 给；
  `services` 的 env 两套前缀都写，两个镜像各读自己那半、取值相同。四支都带
  `-Wall -Wextra`——今天零告警，
  但不 `-Werror`，免得依赖头升级把与回归无关的红压进分支。数据库那一支另有一道报警：
  测试连不上就返回 77，而 ctest 把 77 记成 Skip 并照样打印"100% tests passed"，所以作业
  见到输出里的 `Skipped` 即判失败（真正的失败交给 `set -o pipefail`，测试条数不写死），
  并在构建之前先用 `isql` 打通一次 DSN，把"驱动没装对"与"库有回归"分成两种红。
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
  `src/odbc/native_types.hpp`（`inline` 头，不进 ABI），`column_model` 改存中立 `sql_type`、
  由 `schema_reader` 在它的 ODBC 边界上归一，公开头不再声明 `sql_type_from_native`；`core`
  作业另加一道 shadow：往 include 路径最前放一对读下去即报错的 `sql.h`/`sqlext.h`，再用一次
  反面编译确认它们确实抢在了系统头之前——且要求那次编译非报我们那句 `#error` 不可，编不动
  的编译器同样会"失败"，而那不算守卫生效。两处都改完后再提交一趟，三支作业在 runner 上全绿：
  `core` 带着 shadow 编过，mariadb 腿的 golden 仍逐字节相符——也是按当时的比法。
  全绿之后这支沿服务端又铺开一格，从两条腿变成 2×2 四条腿。本地量到的：两支连接器都能连上
  `8.4.11`，用户是 `caching_sha2_password`、走 TCP、DSN 不需要任何额外键；四格各跑一遍
  抽取、固定同一份 `uniorm-gen`（那份构建早于 `decimal_t`），截掉注释后的输出四格 md5
  相同——格与格的差别全在注释里，代码一处没有。这条规则套今天的工具与今天的 golden
  复算过：MariaDB 那一格五条全绿；`3.1.12` × `8.4` 那一格四条绿（含 `integration_tests`，
  那台服务端开着 `ONLY_FULL_GROUP_BY`），红的一条正是 `.primary_key` 变成 `.column`
  的那处读丢，不必任何开关。所以四条腿这副形状欠 runner 的只剩两样：矩阵选镜像、
  `health_cmd` 按镜像给、`services` 里两套 env 前缀这些新形状，以及 CI 钉的那两支连接器
  在 `8.4` 上做的抽取（本地那支是更老的 `3.1.12`，红因它而在）。
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
一遍、也在 GitHub 上跑绿之后，本清单一度清空；驱动矩阵沿服务端铺成 2×2 之后，又剩下一件事，
见打包条目末尾）：

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
  全绿（两处细节都在 §8）。**待做**：那一趟绿的形状是两条腿；矩阵已沿服务端铺成 2×2，
  四条腿这副还没被 runner 看过——要它确认的是配对本身（矩阵选镜像、按镜像给的健康门、
  `services` 里两套 env 前缀），以及 CI 钉的那两支连接器在真 MySQL 服务端上的那次抽取
  （§8 里四格截注释等值的那趟量的是早于 `decimal_t` 的构建）。

原有路线图：

1. ~~backend 接口提取~~ **已完成（里程碑 1）**：中立接口 + scheme 注册表，
   ODBC 迁移至接口之后，ODBC 改 PRIVATE 链接，核心单测不链接 ODBC
   （见 §5）。**待做**：libpq backend、Oracle OCI backend（见 §5.4）
2. Unit of Work / 脏检查 / 级联
3. ~~批量操作~~ **已完成**：`SQL_ATTR_PARAMSET_SIZE` 数组绑定 + 块取行，
   列式（`batch_writer_iface`）与行式（`bind_batch_params`）双通道（见 §4.5.2）。
   **待做**：OCI backend 的原生数组绑定
4. 异步包装层（libpq backend 可用原生异步）
5. 外键导航 / 关联加载
6. 离线 schema 快照输入（DDL 解析）
7. 迁移脚本生成
8. backend 拆成独立链接目标（如 `uniorm_odbc` / `uniorm_pq`）：多 backend 共存时
   让"只用一家"的部署不必在运行时载入其余驱动（见 §5.1）
9. 能力清单落地：`capabilities` 的四个未读标志各自找到真实消费点，
   并把 `capability_not_supported` 的抛出接上（见 §5.2）
10. ~~池归还时的状态清理~~ **已完成**：`release` 见 `autocommit()` 为假即
    rollback 后复位 autocommit，复位抛异常则淘汰该连接并扣回名额（见 §4.9）；
    兜底不再只挂在 `orm` 借出侧，直接用 `connection_pool::acquire()` 的借用者
    同样拿到干净的连接

## 10. 评审待定点

1. ~~聚合投影自实现 PFR 手法还是依赖 Boost.PFR~~ **已定：自实现 pfr-lite，字段上限 64，不引入 Boost**（见 §4.6）；
2. ~~DECIMAL/NUMERIC v1 默认映射~~ **已定方向：默认取无损一侧**（见 §4.3）。
   动态行取精确定点字面量（无损），`uniorm-gen` 的默认映射生成 `std::string`
   成员，实体/投影随成员类型（`std::string` 即无损）；"可配"原先落在 CMake 选项
   `UNIORM_DECIMAL_DEFAULT` 上，但它只改生成物、不改库，已删除——配置改由
   生成器的 `[types]` / `cpp_type` / `converter` 按列承担（§6.4/§4.4）。
   `decimal_t` 也已落地：`uniorm::decimal_t` 以自带 converter 接入，正走的就是这条
   按列覆写（§4.3 第 2 条）；
3. ~~`orm`（注册表）与 `connection` 的组合方式~~ **已定：`orm` 作为中心入口，内部持有 `connection`**，`db.query().of<T>()`、`db.insert()`、`db.update()` 等统一经 `orm` 调用（见 §4.8）；
4. ~~头文件-only 还是编译库~~ **已定：动态库**（避免 header-only 升级后全量重编），非模板实现进 `libuniorm`，模板代码留头文件（见 §1）。

**四项决策均已定，且都已落到实现。**"决策定了"与"实现到位"此前在第 2 项上分叉
（欠一个 `decimal_t`），如今并拢；仍未做的事只剩 §9 的条目。
