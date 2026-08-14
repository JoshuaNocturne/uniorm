# uniorm v1 设计文档

状态：v1 实现完成（单元测试 + MariaDB 集成测试通过）
日期：2026-08-07

## 1. 目标与范围

uniorm 是一个基于 **ODBC**（而非各数据库专有 C 客户端）的现代化 C++ 数据库访问层。

### v1 范围（In Scope）

- ODBC 句柄的 RAII 封装（env / dbc / stmt）
- 连接管理与最小连接池（固定大小、借还、超时）
- 事务（RAII）
- 参数绑定、语句执行、结果集迭代
- 类型系统：SQL 类型 ↔ 现代 C++ 类型映射，含 converter 扩展点
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
- 批量操作（数组参数绑定）、游标更新
- Schema 迁移管理（migration）

### 基础决策

| 决策项 | 结论 |
|---|---|
| C++ 标准 | C++20 |
| 接口风格 | 纯同步 |
| 平台 | Linux（unixODBC）、Windows（原生 ODBC） |
| 错误处理 | 异常 |
| Unicode | 内部一律 UTF-8，仅在 ODBC 边界转换 |
| 库形态 | 动态库（`libuniorm.so` / `uniorm.dll`），经 `UNIORM_API` 导出宏控制符号可见性；模板密集代码（mapping/query/projection/pfr）保留在头文件 |

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
│ environment / connection / statement 句柄 RAII   │  ODBC 封装层
│ odbc_error / diagnostics                        │  （v1 唯一 backend 实现）
└────────────────────────────────────────────────┘
        ⇡ v2 在此处之上插入 backend 抽象接口（见 §5）
```

依赖方向严格向下；高层不直接触碰 `SQLH*` 句柄类型。ODBC 专有概念（`SQLLEN` / indicator / SQLSTATE / `?` 占位符细节）禁止上浮到语句层公共 API 之上——这条纪律是 v2 多 backend 抽象（§5）的前提。

## 3. 目录结构

```
uniorm/
├── CMakeLists.txt
├── include/uniorm/
│   ├── export.hpp               # UNIORM_API 符号导出宏
│   ├── error.hpp                # 异常体系（不含 odbc_error）
│   ├── unicode.hpp              # UTF-8 ↔ UTF-16
│   ├── value.hpp                # sql_value variant / timestamp
│   ├── types.hpp                # backend 中立 sql_type 枚举 + sql_type_from_native
│   ├── converter.hpp            # 自定义类型转换器（concept has_converter）
│   ├── row.hpp                  # 动态行 + value_cast
│   ├── params.hpp               # 参数容器 + make_sql_value 转换
│   ├── result_set.hpp           # 行式绑定结果集（pimpl）
│   ├── connection.hpp           # 高层 connection
│   ├── transaction.hpp
│   ├── pool.hpp                 # connection_pool / pooled_connection
│   ├── dialect.hpp              # 方言特性（引用符、分页）
│   ├── odbc/                    # ODBC 封装层
│   │   ├── environment.hpp
│   │   ├── connection.hpp
│   │   ├── statement.hpp
│   │   ├── error.hpp            # odbc_error / diagnostics
│   │   └── detail/handles.hpp   # 句柄 RAII、traits
│   ├── detail/
│   │   ├── pfr.hpp              # 自实现聚合体反射（字段数探测 + 展开，上限 64）
│   │   ├── projection.hpp       # 聚合 struct 投影绑定（field_binding 体系）
│   │   ├── traits.hpp           # is_optional_v 等共享 traits
│   │   ├── time.hpp             # chrono ↔ 日历拆分/组装
│   │   └── param_staging.hpp    # 参数绑定的指示器/缓冲暂存
│   ├── mapping/registry.hpp     # 实体映射注册表（含 mapping_builder）
│   └── query/
│       ├── builder.hpp          # query_gateway / query<T>
│       └── expression.hpp       # member_key / predicate / 谓词构造器
├── src/                         # 对应实现（编译进 libuniorm）
├── tools/uniorm-gen/            # 代码生成 CLI
│   ├── main.cpp                 # 参数解析与编排
│   ├── schema_reader.cpp        # ODBC 元数据提取
│   ├── generator.cpp            # model + 配置 → 头文件文本
│   ├── config.cpp               # TOML 子集解析
│   └── naming.cpp               # PascalCase/camelCase 标识符转换
├── tests/unit/                  # 无数据库依赖的单测（unicode/odbc_handles/pfr/
│                                # row/params/expression/registry）
└── docs/design.md
```

## 4. 核心模块设计

### 4.1 ODBC 封装层

RAII、move-only 的句柄包装，屏蔽所有 `SQLFreeHandle` / 错误提取细节。最终 API：

```cpp
namespace uniorm::odbc {

class odbc_error : public uniorm_error {
public:
    struct diagnostic {
        std::string state;      // SQLSTATE，5 字符
        std::int64_t native_code = 0;
        std::string message;
    };
    odbc_error(std::string const& context, std::vector<diagnostic> diags);
    std::vector<diagnostic> const& diagnostics() const noexcept;
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
    void execute();
    bool fetch();                      // 下一行，false = 结果耗尽（容忍 SQL_NO_DATA）
    std::size_t affected_rows() const;
    std::size_t column_count() const;
    // index 均为 1-based；indicator 指向调用方持有、生命周期覆盖执行的 SQLLEN
    void bind_parameter(SQLUSMALLINT index, SQLSMALLINT c_type, SQLSMALLINT sql_type,
                        SQLPOINTER value, SQLLEN buffer_length, SQLLEN* indicator,
                        SQLULEN column_size = 0, SQLSMALLINT decimal_digits = 0);
    void bind_column(SQLUSMALLINT index, SQLSMALLINT c_type, SQLPOINTER value,
                     SQLLEN buffer_length, SQLLEN* indicator);
    void close_cursor();
    void reset();                      // 解绑列并重置参数
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
- 字符串列用 `SQL_C_CHAR`（UTF-8）或 `SQL_C_WCHAR` + 转换，按列类型在绑定时决定；
- 长数据（长 VARCHAR / BLOB）v1 策略：绑定固定缓冲，截断（indicator 超界或 `SQL_NO_TOTAL`）时经 `SQLGetData` 循环重取**完整值**整体替换——MariaDB Connector/ODBC 在截断续读时返回的是全量值而非剩余部分，追加式拼接会重复计数据。

### 4.2 Unicode 策略

- 库内部所有 `std::string` / `string_view` 均为 UTF-8；
- 宽字符 API（`SQLWCHAR`，UTF-16）仅在两种场景使用：驱动只支持宽字符的列/参数、Windows 下的 DSN 连接串；
- 转换集中在 `unicode.hpp`：`utf8_to_utf16` / `utf16_to_utf8`，边界处一次性完成；
- 列读取默认尝试 `SQL_C_CHAR`，驱动拒绝或数据含非 BMP 字符时回退宽字符路径（由实现细节处理，不暴露给用户）。

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

静态映射规则（`types.hpp` 中的 `sql_type_of<T>` / 绑定 traits）：

| SQL 类型 | C++ 类型 |
|---|---|
| BIT | `bool` |
| TINYINT / SMALLINT / INTEGER / BIGINT | `int16_t` / `int32_t` / `int64_t`（TINYINT 也接受 `int8_t`） |
| REAL / FLOAT / DOUBLE | `float` / `double` |
| DECIMAL / NUMERIC | 见下方"DECIMAL 策略"（当前实现：按字符串读取，无损） |
| CHAR / VARCHAR / LONGVARCHAR | `std::string` |
| WCHAR / WVARCHAR / WLONGVARCHAR | `std::string`（UTF-16 → UTF-8） |
| DATE / TIME / TIMESTAMP | `timestamp`（DATE/TIME 补零时间部分后同样落为 `timestamp`；v1 不单独提供日历/时刻类型） |
| BINARY / VARBINARY / LONGVARBINARY | `std::vector<std::byte>` |
| GUID | `std::string`（v1 以字符串形式暴露） |

**DECIMAL 策略（两级选择）**：

1. **全局默认**：CMake 选项 `UNIORM_DECIMAL_DEFAULT`（`string`（默认，无损）或 `double`）以编译期宏 `UNIORM_DECIMAL_AS_STRING` / `UNIORM_DECIMAL_AS_DOUBLE` 注入公开接口；当前实现中 DECIMAL/NUMERIC 一律按 `std::string` 读取（无损），`decimal_t` 别名随宏切换属规划内收尾项；
2. **逐列覆写**：任意列可通过 `converter<C, S>` 特化映射到自定义类型（含 `double` / `std::string` / 第三方 decimal 类）；`uniorm-gen` 的 `[types]` 与 `[tables.*.columns.*] cpp_type` 配置同样生效。

可空列对应 `std::optional<T>`；绑定与取值逻辑对 `optional` 做特化（indicator = `SQL_NULL_DATA`）。

### 4.4 Converter（自定义类型扩展点）

```cpp
namespace uniorm {

template <class Cpp, class Sql>
struct converter;                      // 用户特化

// 示例：enum ↔ 字符串
template <>
struct converter<Status, std::string> {
    static std::string to_db(Status s);
    static Status from_db(std::string_view v);
};

} // namespace uniorm
```

存在 `converter<C, S>` 特化时，实体字段 `C` 即可绑定到 SQL 类型为 `S` 的列。查询构建器对 converter 字段同样可用（比较值先经 `to_db` 转换）。

### 4.5 语句与结果集

最终 API：

```cpp
namespace uniorm {

struct column_info {
    std::string name;
    sql_type type;                 // backend 中立枚举（types.hpp）
    std::size_t display_size;
    bool nullable;
};

// 行式绑定结果集；move-only，pimpl；由 connection::execute 创建
class result_set {
    bool next();                   // 下一行；false = 结果耗尽
    row current();                 // 物化当前行为动态 row
    std::size_t column_count() const;
    column_info const& column(std::size_t index) const;
};

// sql_value → T：精确匹配优先，整数宽度间范围检查收窄，
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

// 高层连接（使用 odbc::shared_environment()）
class connection {
    explicit connection(std::string_view connection_string);
    void close();
    bool is_open() const noexcept;

    result_set execute(std::string_view sql, params const& p = {});
    std::size_t execute_update(std::string_view sql, params const& p = {});

    // 批量插入（见 4.5.2）：多行 VALUES + 事务包裹，返回插入行数
    std::size_t insert_batch(std::string_view table,
                             std::vector<std::string> const& columns,
                             std::vector<params> const& rows);
    template <class Entity>
    std::size_t insert(orm const& registry, std::vector<Entity> const& rows);

    // 聚合投影路径（见 4.6）：按列序绑定，无注册
    template <detail::aggregate_projection T>
    std::vector<T> query(std::string_view sql, params const& p = {});

    query_gateway query(orm& registry);   // 实体查询入口，见 4.8
    transaction begin();                  // 见 4.9
    std::string dbms_name() const;        // SQL_DBMS_NAME，方言推断输入

    // 预编译语句缓存观测（见 4.5.1）
    unsigned long long statement_cache_hits() const;
    unsigned long long statement_cache_misses() const;
    std::size_t statement_cache_size() const;
    void clear_statement_cache();
};

} // namespace uniorm
```

`params` 支持 `execute(sql, params{18, "alice"})` 这类写法；绑定实现返回按值的
暂存对象（`detail::param_staging`），调用方在执行期间持有，规避了 pimpl 下
不完整类型的析构问题并保证异常安全。

v1 采用**行式绑定**（每列 `SQLBindCol`，逐行 `SQLFetch`）；列式绑定留给批量操作（v2）。

#### 4.5.1 预编译语句缓存

`connection` 内置按 SQL 文本为键的 LRU 语句缓存（`detail::statement_cache`，
容量 64），对调用方完全透明：`execute` / `execute_update` / 聚合投影
`query<T>` / 实体查询（`execute_with`）全部走 checkout/check-in 模型：

- 缓存**只存空闲句柄**：执行前取出（命中则 `statement::reset()`——
  `SQLFreeStmt` 关游标/清参数/解列绑定——后直接复用），用完归还；
  使用中的句柄不在缓存内，同一 SQL 并发执行互不干扰（各自新建）
- `execute_update` 执行完立即归还；`execute` 的 `result_set` 携带
  check-in 闭包（`weak_ptr` 引用缓存状态），析构时归还——缓存先于
  连接句柄销毁，闭包在连接已销毁时静默丢弃句柄
- 归还时该 SQL 已有条目（并发 checkout 期间产生过新建）则丢弃；
  超容量从 LRU 尾部淘汰
- `close()` 先清空缓存再断开，保证语句句柄先于 DBC 释放

边界：DDL（DROP/CREATE）会使同表旧 prepare 失效，驱动报错后句柄即弃用，
重试自然走 miss 路径；需要立即失效可调用 `clear_statement_cache()`。
`hits()/misses()` 计数器供测试与观测。

#### 4.5.2 批量插入

两个入口：

```cpp
// 实体版（主接口）：列名与顺序来自注册映射，字段经注册的 read 闭包提取
std::size_t n = conn.insert(registry, users);      // users: std::vector<User>

// 动态版（无映射逃生口）
conn.insert_batch("user", {"name", "age"},
                  {params{"alice", 30}, params{"bob", nullptr}});
```

实现要点：

- 生成 `INSERT INTO <表> (<列…>) VALUES (?, ?…), (?, ?…)` 多行占位符语句，
  标识符一律经 `dialect::quote_identifier`；每批占位符数不超过 4096
  （低于 MySQL/MariaDB 约 65535 的语句上限），按 `4096 / 列数` 分批
- 全部批次包在**一个事务**里；任一批失败由 transaction 析构回滚
- 动态版在执行前校验每行 `params` 个数等于列数（个数不匹配抛
  `uniorm_error`）；类型一致性交给驱动/服务器判断
- 实体版结构免验证：容器元素同类型，列集合在注册期固定；
  `std::optional` 空值经 read 闭包转为 `monostate`（NULL），走现有参数绑定通道
- `column_meta` 持有三类闭包：`write`（populate 用）、`read`（insert 用）、
  `make_binding`（查询直绑工厂，见 §4.8）

### 4.6 聚合 struct 投影（零注册路径）

无需任何映射注册，按列序绑定聚合体：

```cpp
struct user_row {
    int64_t id;
    std::string name;
    std::optional<int32_t> age;
};

auto rows = conn.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", {18});
for (auto const& u : rows) { /* ... */ }
```

实现：自实现 `pfr_lite`（`detail/pfr.hpp`，不引入 Boost），含两个核心设施——
`field_count<T>`（万能转换类型 + `requires` 表达式探测聚合体字段数）与
`tie_aggregate(T&)`（按字段数 `if constexpr` 分派的结构化绑定展开，由宏生成至上限）。
要求：`std::is_aggregate_v<T>`（入口 concept 约束）、字段数 ≤ 64（超限编译期报错）、
字段类型满足 4.3 的映射或存在 converter；嵌套聚合体计为一个字段，需经 converter 绑定。

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
    member_key key;
    std::function<void(void*, sql_value const&)> write;  // populate 用写闭包
    std::function<sql_value(void const*)> read;          // insert 用读闭包
    // 查询物化：把该列直接 SQLBindCol 到 obj 的成员上（见 §4.8）
    std::function<std::unique_ptr<detail::field_binding>(void*)> make_binding;
};

struct entity_meta {
    std::string table;
    std::vector<column_meta> columns;
    std::vector<member_key> ignored;

    std::string const& column_name(member_key const& key) const;  // 未注册抛 mapping_error
    void populate(void* obj, row const& r) const;                 // 按列名写回对象
};

// 成员类型须满足 readable_member：bool/int8~64/double/string/bytes/timestamp
// 或上述类型的 std::optional（编译期 static_assert）
template <class T>
class mapping_builder {
    mapping_builder& column(std::string_view column, M T::*member);
    mapping_builder& primary_key(std::string_view column, M T::*member);
    mapping_builder& ignore(M T::*member);                 // transient
};

class orm {                        // 非线程安全，按线程/会话持有
    template <class T> mapping_builder<T> map(std::string_view table);
                                   // 重复注册抛 mapping_error
    template <class T> entity_meta const& meta() const;    // 未注册抛 mapping_error
    entity_meta const* find(std::type_index type) const;
    std::size_t size() const noexcept;

    void validate(connection& conn, validation_mode mode = validation_mode::strict);
    // 逐实体经 SQLColumns 对账：
    //  - 表不存在（结果集为空）       → mapping_error
    //  - 列缺失                       → mapping_error
    //  - 列可空但成员非 optional      → strict 抛 mapping_error / lenient 放行
};

} // namespace uniorm
```

成员指针的类型擦除：注册时经 `make_column_meta` 捕获 `write` 闭包
（`[member](void* obj, sql_value const& v) { static_cast<T*>(obj)->*member = value_cast<M>(v); }`），
运行时物化不再需要模板。查询结果按**列名**（而非列序）写回，
天然规避 SELECT 列序与 struct 字段序不一致的问题。

### 4.8 成员指针查询构建器

```cpp
auto users = conn.query(orm)
    .of<User>()
    .where(gt(&User::age, 18) && eq(&User::status, Status::Active))
    .order_by(&User::name, direction::desc)
    .limit(50)
    .all();                        // std::vector<User>

auto count = conn.query(orm).of<User>()
    .where(col(&User::age) > 18)
    .count();

auto opt = conn.query(orm).of<User>()
    .where(col(&User::id) == 42)
    .one();                        // std::optional<User>
```

谓词写法说明：不使用 `&User::id == 42` 这类直接对成员指针重载运算符的形式。
原因有二：C++20 下模板化 `operator==` 会与"翻转候选"重写规则冲突产生歧义；
且 GCC 11 对成员指针操作数不做 ADL，运算符在多数编译器上根本找不到。
因此提供命名函数 `eq/ne/lt/le/gt/ge`，以及 `col(&User::x)` 包装后的中缀写法；
`&&` / `||` / `in` / `is_null` / `like` 作用于 `predicate` 本身，不受影响。

连接前置是刻意为之：让调用方始终明确"操作发生在哪条连接上"，`orm` 只作为元数据注册表传入。
`conn.query(orm)` 返回轻量网关（持有连接与注册表的引用），`.of<T>()` 产出 `query<T>` 构建器；
事务语义自然跟随连接（在 `transaction` 作用域内执行的查询即处于该事务中）。

物化路径：`all()`/`one()` 不经过 `result_set`/`row`/`sql_value`，而是把结果列
**直接 `SQLBindCol` 到实体字段上**。注册时 `column_meta` 除 `write`/`read` 闭包外
再生成 `make_binding` 工厂（`detail::make_field_binding` 按成员类型选择数值直绑、
字符串/二进制定长缓冲 + 截断重读、时间戳暂存、`std::optional` 空值复位等绑定策略）。
`render_select` 保证 SELECT 列序与 `meta.columns` 注册序一致，`entity_binding<T>`
按序号绑定到一个原型对象；每次 `SQLFetch` 后 `finalize()` + `std::move(proto_)`
产出一行（与聚合投影 `projection<T>::take()` 同一机制）。
`connection` 提供私有逃生舱 `execute_with(sql, params, fn)`：
prepare → 绑定参数 → execute，随后把活动语句交给 `fn`（`query<T>` 为友元）。
相比经 `row` 物化，每行省去 `sql_value` 构造与按名查找，数值列零拷贝、
变长列少一次中转拷贝；`count()` 仍走 `result_set` 单值路径。

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
class query {
    query& where(predicate p);                       // 多个 where 以 AND 连接
    template <class M> query& order_by(M T::*member, direction dir = direction::asc);
    query& limit(std::size_t n);
    query& offset(std::size_t n);
    std::string build_select() const;                // 干跑生成 SQL，不执行
    std::vector<T> all();
    std::optional<T> one();                          // 强制 limit 1
    std::int64_t count();
};

class query_gateway {                                // conn.query(orm) 的返回值，不持有所有权
    template <class T> query<T> of();                // 未注册实体抛 mapping_error
    connection& conn() const;
    orm& registry() const;
    dialect const& sql_dialect() const;              // 首次调用时探测并缓存
};
```

v1 支持的谓词：`= != < <= > >=`、`&&`、`||`、`in(...)`、`is_null` / `is_not_null`、`like`；不支持子查询、join（join 场景引导用户走原生 SQL + 投影）。

### 4.9 事务

最终 API：

```cpp
class transaction {                    // move-only，RAII
public:
    explicit transaction(connection& conn);   // 关闭 autocommit，进入事务
    ~transaction();                    // 仍 active 则 rollback（析构不抛异常）

    void commit();                     // 提交并恢复 autocommit
    void rollback();                   // 回滚并恢复 autocommit
    bool active() const noexcept;
};

transaction connection::begin();       // 等价于 transaction(conn)
```

v1 不支持嵌套事务/savepoint；连接归还池前强制回滚未决事务。

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
    explicit connection_pool(pool_options);   // 使用 shared_environment()
    pooled_connection acquire();       // 懒创建；超时抛 pool_timeout；
                                       // 建连失败时归还名额并原样重抛；
                                       // 空闲超过 max_idle_time 的连接直接丢弃
    std::size_t capacity() const;
    std::size_t idle_count() const;
    unsigned long long heartbeats_executed() const;
};

class pooled_connection {              // move-only；析构时归还池
    connection& get() noexcept;
    connection* operator->() noexcept;
    explicit operator bool() const noexcept;   // 是否仍持有有效连接
};
```

全局调度线程（所有池共享一个，首个启用维护的池创建时启动，注册表清空后退出；
`heartbeat_interval > 0` 的池注册为 `weak_ptr`，进程内不随池数量增长线程）：

- 每个周期先把空闲超时的连接移出借出列表并**在锁外断开**
  （`SQLDisconnect` 可能阻塞，不能持锁执行）
- 其余空闲连接移到锁外逐个执行 `heartbeat_sql`；执行失败视为连接已死，
  直接丢弃；成功则带着原 `released_at` 放回（空闲时长跨心跳累计）
- 丢弃会归还名额（`created--`）并 notify，等待者可触发懒创建
- 心跳执行期间不持有池锁，`acquire/release/idle_count` 不被阻塞
- 调度循环按各池 `next_tick` 最早截止时间等待；池析构即注销，
  `weak_ptr` 失效保证维护过程不会触及已销毁的池

v1 不做：动态伸缩。池须比所有借出的连接长寿。

## 5. Backend 抽象：多后端规划与 v1 纪律

背景：libpq 与 Oracle OCI 在实际项目中都存在必须独立使用原生 API 的场景（PG：COPY、LISTEN/NOTIFY、异步 I/O；Oracle：OCI 数组绑定、高级特性等），这些是 ODBC 无法覆盖的。因此 uniorm 采用"通用层打底 + 原生特性逃生舱口"策略，而非试图把各库特性塞进统一 API。

### 5.1 v1 边界纪律（立即生效）

v1 不实现 backend 抽象接口（单一实现下抽象必然抽错），但执行以下纪律，保证 v2 提取接口是可控重构而非重写：

- ODBC 类型（`SQLH*`、`SQLLEN`、indicator、SQLSTATE）不得出现在语句层及以上层级的公共 API 中；
- SQL 方言差异集中在 `dialect`，占位符统一以 `?` 语义表达，backend 负责翻译成各自风格（libpq `$1`、OCI `:1`）；
- `transaction`、`connection_pool`、`result_set` 只依赖通用语义（open / prepare / bind / execute / fetch / 列元数据）；
- 单元测试中对语句层以上的测试不得链接 ODBC 头文件。

### 5.2 v2 backend 接口（方向性设计，不在 v1 实现）

```cpp
namespace uniorm::backend {

struct capabilities {
    bool streaming;          // 流式/大结果集分批读取
    bool async_io;           // 原生异步
    bool copy_protocol;      // PG COPY 类批量协议
    bool notifications;      // PG LISTEN/NOTIFY 类事件
    bool array_binding;      // OCI 数组绑定类批量操作
};

struct statement_iface { /* prepare / bind / execute / fetch / column_meta */ };
struct connection_iface { /* open / close / begin / commit / rollback /
                             capabilities() / native_handle() / extension() */ };

} // namespace uniorm::backend
```

核心 API（查询构建器、映射、池、事务）只依赖接口；能力不足时抛清晰错误，不静默降级。

### 5.3 原生特性通道

**原生句柄逃生舱口**——保证"ODBC 做不到的事永远有路可走"：

```cpp
auto* pg = conn.native_handle<libpq_backend>();    // PGconn*
PQputCopyData(pg, ...);                            // 用户自行驱动原生操作
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

独立 CLI，构建期经 CMake custom command 调用，活连接目标数据库：

```
uniorm-gen --dsn=<dsn> | --connection-string=<str>
           --config=uniorm.toml
           --out=<dir>
           [--tables=a,b,c]            # 可选过滤
```

### 6.2 Schema 提取（纯 ODBC 元数据）

- `SQLTables` → 表清单（支持 schema/catalog 过滤）
- `SQLColumns` → 列名、SQL 类型、长度、可空、默认值
- `SQLPrimaryKeys` → 主键
- `SQLForeignKeys` → 外键（v1 仅记录，不生成关联导航）
- `SQLStatistics` → 索引（可选输出注释）

### 6.3 生成物

每库一个头文件 `<name>_schema.hpp`，内容为：

1. `struct`（表名 → PascalCase 类名，列名 → camelCase 成员名，命名规则可配置）；
2. 可空列 → `std::optional<T>`；
3. `inline void register_<name>_schema(orm&)` 注册全部映射；
4. SQL 类型 ↔ C++ 类型映射遵循 4.3 表，可被配置覆写。

生成物只输出到构建目录，视为不可手改。

### 6.4 配置文件（TOML）

```toml
[types]                              # 全局 SQL 类型 → C++ 类型覆写
"NUMERIC(10,2)" = "money"
"TIMESTAMP"     = "std::chrono::system_clock::time_point"

[tables.t_user]
class = "User"                       # 类名覆写
skip = false

[tables.t_user.columns.status]
cpp_type = "Status"                  # 单列类型覆写
converter = "status_converter"       # 指定 converter 特化名
```

## 7. 错误体系总览

```cpp
uniorm_error : std::runtime_error    // 基类（error.hpp）
├── unicode_error                    // UTF-8/UTF-16 转换遇到非法输入
├── column_not_found                 // 动态行按名取值失败
├── type_mismatch                    // value_cast/get<T>/参数归一化失败
├── mapping_error                    // 映射/校验：重复注册、未注册、表列缺失、可空不匹配
└── pool_timeout                     // 池获取超时

odbc::odbc_error : uniorm_error      // ODBC 层（odbc/error.hpp），携带 diagnostics 列表
```

## 8. 测试策略

- **单元测试**（无数据库，已实现）：`test_unicode`（UTF-8/16 往返与非法输入）、`test_odbc_handles`（句柄 RAII）、`test_pfr`（字段数探测/展开/concept 负例）、`test_row`（value_cast/收窄/optional）、`test_params`（值归一化）、`test_expression`（谓词 SQL 生成、方言、分页）、`test_registry`（映射注册/populate/read 闭包/错误路径）、`test_gen_config`
  （TOML 子集解析正例/错误行号/非法键）、`test_gen_output`（命名转换边界
  + 生成器快照与覆写/跳表/错误路径）；
- **集成测试**（已实现，DSN/凭据由 `UNIORM_IT_DSN` / `UNIORM_IT_USER` / `UNIORM_IT_PWD` 指定，凭据以 `UID`/`PWD` 写进连接串；连不上时 ctest SKIP）：execute/params 往返、动态行、聚合投影（含长字符串与 timestamp）、orm validate（含 strict 失败路径）、查询构建器全谓词与分页、事务 commit/rollback/析构回滚、批量插入（实体版含 NULL/超批分批、动态版、参数个数校验）、语句缓存（hit/miss 计数、流式 result_set 借出期间并发 miss、清空）、连接池借还与超时、连接池维护（心跳保活计数、空闲超时驱逐、失败心跳丢弃）；后续按库加条件标签覆盖方言与类型怪癖；
- **性能基准**（已实现，ctest 标签 `perf`，`tests/perf/test_perf.cpp`）：
  连不上库时 SKIP；行数由 `UNIORM_PERF_ROWS` 指定（默认 10000）。
  覆盖批量插入吞吐，以及三条查询物化路径的对比：实体直绑
  （`query<T>::all()`）、聚合投影（`conn.query<Row>`）、动态行
  （`result_set`/`row`/`sql_value`），另含 `one()`/`count()` 单行延迟；
  每项取 best-of-3，输出耗时与 krows/s。
  另含**纯 ODBC 基线**（不经 uniorm，直接操作句柄，仅保留与 uniorm
  相同调用形式的项目）：与 `insert_batch` 逐调用对齐的多行 VALUES
  批量插入（同样 4096 占位符分批、逐值 `SQLBindParameter`、单事务）、
  `SQLBindCol` + `SQLFetch` 全表扫描（对应实体直绑与动态行路径）、
  单行 LIMIT 1（对应 `one()`），用于衡量 uniorm 抽象层的额外开销
- **`uniorm-gen` 端到端**（已实现，`gen_e2e_tests`，连不上库时 SKIP）：
  夹具表（含 PK/FK/索引/DECIMAL/DATETIME）→ 工具生成 → 与检入 golden
  头文件逐字节比对；golden 本身被编译进测试，执行注册 +
  `validate(strict)` + 构建器 `count()`，覆盖"生成 → 编译 → 注册 →
  校验"全链路。golden 假定默认 `UNIORM_DECIMAL_DEFAULT=string`。

## 9. v2 路线图（不在本次范围）

1. **backend 接口提取 + libpq backend + Oracle OCI backend**（见 §5，已确认需求）
2. Unit of Work / 脏检查 / 级联
3. 批量操作（`SQL_ATTR_PARAMSET_SIZE` 数组绑定；OCI backend 可用原生数组绑定）
4. 异步包装层（libpq backend 可用原生异步）
5. 外键导航 / 关联加载
6. 离线 schema 快照输入（DDL 解析）
7. 迁移脚本生成

## 10. 评审待定点

1. ~~聚合投影自实现 PFR 手法还是依赖 Boost.PFR~~ **已定：自实现 pfr-lite，字段上限 64，不引入 Boost**（见 §4.6）；
2. ~~DECIMAL/NUMERIC v1 默认映射~~ **已定：`decimal_t` 全局可配（默认 `std::string`），逐列可经 converter 覆写**（见 §4.3）；
3. ~~`orm`（注册表）与 `connection` 的组合方式~~ **已定：连接前置 `conn.query(orm).of<T>()`**，让调用方明确操作所在连接（见 §4.8）；
4. ~~头文件-only 还是编译库~~ **已定：动态库**（避免 header-only 升级后全量重编），非模板实现进 `libuniorm`，模板代码留头文件（见 §1）。

**全部待定点已闭环。**
