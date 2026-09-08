# uniorm v1 设计文档

状态：v1 实现完成（单元测试 + MariaDB 集成测试 + 性能基准通过）
日期：2026-09-08（本版按当前代码逐节核对，偏差集中记于"已知缺口"）

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

以下条目在本文档中是设计契约，但代码只落地了一部分。列在此处而非埋在
正文，是为了让"文档承诺 = 当前实现"这条约束成立（§3.1/§4.2/§4.3/§10
已就地标注）。只剩 DECIMAL 一条：它对外的承诺（动态行无损）已兑现，
未做的只有 `decimal_t` 那一层解析。

另有一处较小的偏差体量不足以单列，直接在正文就地写实并进了 §9："不碰 ODBC"
只到链接行为止（§5.1）。

1. **DECIMAL 的动态路径**（§4.3）：`result_set`/`row` 现把 `sql_type::decimal`
   归入 `slot_kind::text`（`src/result_set.cpp`），以 `SQL_C_CHAR` 绑定，动态行取到
   驱动给出的**精确定点字面量**（含 scale，如 `0.1000`），不再经 `SQL_C_DOUBLE`
   舍入。`value_cast` 按需把字面量解析回算术目标：`get<double>` 仍可用（解析后仍受
   double 精度所限），`get<std::int64_t>` 对 scale-0 列成立、对带小数或超 int64
   范围的字面量抛 `type_mismatch`（与既有 double→整数拒绝一致；`sql_value` 不带列
   类型，故这条解析对任何恰为数字的文本生效，不限于 decimal 列）。`column_info` 新增
   `scale`（取自 `SQLDescribeCol` 的 DecimalDigits），`display_size` 对 decimal 即
   precision。无损读取仍推荐实体/投影侧声明 `std::string` 或 `db_type = std::string`
   的 converter（§4.4），两者按 `SQL_C_CHAR` 直绑。
   剩余欠账：真正的 `decimal_t`（尾数 + scale）尚未做——字面量已进进程，它只是其上
   一层解析，可后置。（原先的 CMake 选项 `UNIORM_DECIMAL_DEFAULT` 及其派生宏已删除：
   库代码从不读它，只有 `uniorm-gen` 的默认映射读，而那个默认现在恒为无损一侧，
   要 `double` 走生成器配置的 `[types]` / `cpp_type`，见 §4.3/§6.4。）

DECIMAL 条的剩余部分（`decimal_t`）已进 §9 路线图。本节原先还有一条"ODBC 宽字符
路径未使用"，已按它自己写下的处置意见了结：确认不做，删掉零调用者的实现（§4.2）。

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
│   ├── types.hpp                # backend 中立 sql_type 枚举 + sql_type_from_native / sql_type_name + column_info
│   ├── converter.hpp            # 自定义类型转换器（concept has_converter，§4.4）
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
│       └── error.hpp            # odbc_error / diagnostics
├── tools/uniorm-gen/            # 代码生成：uniorm_gen_core(STATIC) + uniorm-gen(CLI)
│   ├── main.cpp                 # 参数解析与编排（唯一进 CLI 的源文件）
│   ├── schema_reader.cpp/.hpp   # ODBC 元数据提取（直连私有句柄层）
│   ├── generator.cpp/.hpp       # model + 配置 → 头文件文本
│   ├── config.cpp/.hpp          # TOML 子集解析
│   ├── naming.cpp/.hpp          # PascalCase/camelCase 标识符转换
│   └── schema_model.hpp         # 中间 schema 模型（生成器输入）
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
├── .clang-format                # Google 风格基线（未挂 hook，无 CI）
└── .gitignore
```

公开/私有边界按"外部消费者是否需要"判定：凡出现在 `uniorm/uniorm.hpp` 或
`uniorm/mapping/registry.hpp`（生成代码的唯一依赖）传递闭包内的头文件留在 `include/`，
其余下沉到 `src/`，与自己的实现 `.cpp` 贴邻，用引号相对名互相引用。于是 `src/`
不在 `uniorm` 目标的任何 include 路径上（同目录引用无需路径），只有确实需要跨目录取用
私有头的四个目标显式 `-I src`（PRIVATE）：`uniorm_gen_core` 与 `uniorm-gen`
（直调 `SQLTables` / `SQLColumns` 等目录函数）以及两个白盒单测。公开头一旦
`#include` 私有头便无法解析，边界由编译器强制；`<sql.h>` 现仅出现在 `src/odbc/`
之下，对外头文件既不带驱动类型，也不带 ODBC 链接依赖（`ODBC::ODBC` 是 PRIVATE）。

### 3.1 安装与集成

`cmake --install build --prefix <p>` 产出四类文件（目录名取自 `GNUInstallDirs`，
64 位 RHEL/Fedora 上 `<libdir>` 解析为 `lib64`）：

| 位置 | 内容 |
|---|---|
| `<libdir>/` | `libuniorm.so.<VERSION>` 加 `SOVERSION`（`0.1`）与裸名两级符号链接；Windows 下 DLL 走 RUNTIME、导入库走 ARCHIVE |
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
  伪装成对外 API。它的 `DT_NEEDED` 写死 `libuniorm.so.0.1`，而构建树留下的
  `RPATH` 是绝对路径，故 ELF 上以 `INSTALL_RPATH` 改写成 `$ORIGIN/../<libdir>`
  ——装到哪个 prefix 就找哪个 prefix，与库同树发布时版本必然对上。

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
`sql_type_from_native()`（`types.hpp`）把驱动的 native code 归一为中立 `sql_type`；
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
| DECIMAL / NUMERIC | 见下方"DECIMAL 策略"（实体/投影随字段类型：`std::string` 即无损；动态行取精确定点字面量，`get<std::string>` 无损、`get<double>` 按需解析） |
| CHAR / VARCHAR / LONGVARCHAR | `std::string` |
| WCHAR / WVARCHAR / WLONGVARCHAR | `std::string`（走窄字符路径，转换归驱动；库内未接线，见 §4.2） |
| DATE / TIME / TIMESTAMP | `timestamp`（DATE/TIME 补零时间部分后同样落为 `timestamp`；v1 不单独提供日历/时刻类型） |
| BINARY / VARBINARY / LONGVARBINARY | `std::vector<std::byte>` |
| GUID | `std::string`（v1 以字符串形式暴露） |

**DECIMAL 策略（默认无损，覆写按列）**：

1. **默认**：动态行一律按 `SQL_C_CHAR` 取精确定点字面量（`slot_kind::text`），
   `get<std::string>` 得字面量，`get<double>` 经 `value_cast` 解析（仍受 double
   精度所限）；`uniorm-gen` 把 DECIMAL/NUMERIC 列一律生成为 `std::string` 成员，
   实体/投影侧因此也按 `SQL_C_CHAR` 直绑。这个默认曾经可配（CMake 选项
   `UNIORM_DECIMAL_DEFAULT`，`string` / `double`），但库代码从不读它派生的宏，
   只有生成器的默认映射读，一个"只改生成物"的构建期旋钮不足以承担配置项的名义，
   故已删除；`decimal_t` 别名不存在（见已知缺口的 DECIMAL 条）；
2. **逐列覆写**：要 `double` 或第三方 decimal 类，走生成器配置的 `cpp_type` /
   `[types]`（§6.4，取值限于本节的可绑定集合），或 `converter<C>` 特化（§4.4）——
   其 `db_type` 为 `std::string` 时即无损；`uniorm-gen` 侧的入口是
   `converter = "..."`，生成的成员就是该域类型，头文件以
   `static_assert(uniorm::has_converter<C>)` 要求特化存在。

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
// 支持 std::optional<U>；失败抛 type_mismatch
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
- 调度循环按各池 `next_tick` 最早截止时间等待；池析构即注销，
  `weak_ptr` 失效保证维护过程不会触及已销毁的池

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
（§4.4）。converter 只有逐列入口，没有全局覆写。

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
  `test_gen_output`（命名转换边界 + 生成器快照与覆写/跳表/converter 生成/错误路径）；
  后两个只在 `UNIORM_BUILD_TOOLS` 打开时编入（同时定义 `UNIORM_TEST_GEN`），
  因为它们要链 `uniorm_gen_core`；
- **集成测试**（已实现，DSN/凭据由 `UNIORM_IT_DSN` / `UNIORM_IT_USER` / `UNIORM_IT_PWD` 指定，凭据以 `UID`/`PWD` 写进连接串；连不上时 ctest SKIP）：execute/params 往返、动态行（含 `connection::execute` 显式块取行大小 0 退回逐行）、DECIMAL 动态路径（`DECIMAL(20,4)` / `(20,0)` / `(38,0)` 取回精确定点字面量与 `column_info::scale`，按需解析成 double、scale-0 解析成 int64，带小数或超 int64 范围的字面量抛 `type_mismatch`，NULL 仍报 `is_null`）、聚合投影（含长字符串与 timestamp）、converter 往返（批量插入、实体物化含 NULL、
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
  （`golden/gen_it.toml`，其 `converter` 键让 `note` 生成为域类型）生成 →
  与 golden 头文件逐字节比对；golden 本身被编译进测试，执行注册 +
  `validate(strict)` + 构建器 `count()` + 实体写入与物化读回，覆盖"生成 → 编译 →
  注册 → 校验 → 读写"全链路。
- **安装冒烟**（已实现，`install_smoke`，`cmake -P` 脚本，只在 top-level 且非交叉编译
  时注册，不需要数据库）：`cmake --install` 到 `<build>/install_smoke/prefix` → 核对
  config/targets/头文件/库都已就位，且装出的头文件集合与 `include/uniorm` 逐一相符 →
  配置并构建 `tests/install/` 这个外部工程（刻意不设 `CMAKE_CXX_STANDARD`，靠导出目标
  携带），跑起来的消费者调用 .so 里的 `dialect::detect`、`parse_scheme`、未注册 scheme
  抛 `unknown_scheme`，ODBC 构建下另核对 `"odbc"` 已随载入自注册 → 再以 `99.0.0` 配置
  一次，要求被 `SameMinorVersion` 拒掉 → 最后运行装出来的 `uniorm-gen --help`，它只可能
  经 `$ORIGIN/../<libdir>` 载到库，故 RPATH 改写一并验了。

## 9. v2 路线图

**v1 欠账**（"已知缺口"只剩 DECIMAL 一条，其欠的部分也就是一个从未落地的类型
（`decimal_t`）；宜排在 v2 新特性之前）：

- ~~ODBC 宽字符路径（§4.2）~~ **已按该条目自己给出的第二条路了结**：确认不做，
  删掉零调用者的 `utf8_to_utf16` / `utf16_to_utf8` 与只有它们会抛出的公开类型
  `unicode_error`，边界策略写实为"一律窄字符、编码转换留在驱动侧"。何时值得重新
  引入，见 §4.2 末尾的触发条件。
- DECIMAL 无损动态路径（§4.3）：~~动态行按 `SQL_C_CHAR` 取原始字面量、
  `column_info` 保留 `scale`、清掉零引用的 `UNIORM_DECIMAL_AS_STRING`~~
  **已完成**——`sql_type::decimal` 归入 `slot_kind::text`，字面量精确进进程，
  `value_cast` 按需解析回算术目标；`scale` 目前只对外暴露，库内无消费点；
  `UNIORM_DECIMAL_DEFAULT` 连同派生宏一并删除，生成器的 DECIMAL 默认恒为
  `std::string`，要 `double` 走配置的 `[types]` / `cpp_type`。**待做**：真正的
  `decimal_t`（尾数 + scale）；
- ~~打包~~ **已完成（§3.1）**：`install(TARGETS/EXPORT)` + config/version 文件 +
  `VERSION`/`SOVERSION`，`$<INSTALL_INTERFACE:include>` 与 `project(VERSION)` 已
  生效，外部工程可用 `find_package(uniorm CONFIG)` 接入，`uniorm-gen` 也随
  `UNIORM_BUILD_TOOLS` 装进 `<bindir>`。**待做**：把这套验证接进 CI——安装面如今
  有 `install_smoke`（§8）在一条 `ctest` 里跑完，但没有流水线在制品产出后替它报警，
  装错仍只有本机知道。

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
   动态行取精确定点字面量（无损），`uniorm-gen` 恒生成 `std::string` 成员，
   实体/投影随成员类型（`std::string` 即无损）；"可配"原先落在 CMake 选项
   `UNIORM_DECIMAL_DEFAULT` 上，但它只改生成物、不改库，已删除——配置改由
   生成器的 `[types]` / `cpp_type` / `converter` 按列承担（§6.4/§4.4）。仍缺
   `decimal_t`，见"已知缺口"的 DECIMAL 条与 §9 欠账；
3. ~~`orm`（注册表）与 `connection` 的组合方式~~ **已定：`orm` 作为中心入口，内部持有 `connection`**，`db.query().of<T>()`、`db.insert()`、`db.update()` 等统一经 `orm` 调用（见 §4.8）；
4. ~~头文件-only 还是编译库~~ **已定：动态库**（避免 header-only 升级后全量重编），非模板实现进 `libuniorm`，模板代码留头文件（见 §1）。

**四项决策均已定。**注意"决策定了"不等于"实现到位"——第 2 项仍欠一个
`decimal_t`，见"已知缺口"。
