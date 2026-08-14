# uniorm

[中文文档](README.zh.md)

A modern C++20 database access layer built on ODBC. Instead of depending on
vendor-specific C clients, uniorm talks to any database with an ODBC driver
through one unified interface, and layers entity mapping, a member-pointer
query builder, transactions, and connection pooling on top.

See [docs/design.md](docs/design.md) for the full design.

## Features

- **Synchronous API with an exception-based error hierarchy** — every failure
  throws from the `uniorm_error` tree
- **UTF-8 everywhere internally** — conversions happen only at the ODBC
  boundary (UTF-16)
- **Prepared statements + bind variables** — user values always go through
  `SQLBindParameter`; no string interpolation, no injection
- **Transparent statement cache** — an LRU cache keyed by SQL text skips
  re-prepare on repeated execution
  (observability: `statement_cache_hits()/misses()/size()`)
- **Three access levels**:
  - Raw SQL: `execute` / `execute_update` with `params`
  - Aggregate projection: `conn.query<Row>(sql)` maps columns onto a plain
    struct with zero registration
  - Entity mapping: explicit registry plus a type-safe member-pointer query
    builder, `conn.query(orm).of<T>()`
- **Direct entity binding** — `query<T>::all()/one()` bind result columns
  straight onto entity fields (`SQLBindCol`), bypassing row materialization
- **Batch insert** — `conn.insert(orm, rows)` / `conn.insert_batch(...)`:
  multi-row VALUES, automatic chunking, wrapped in a transaction
- **RAII transactions** — automatic rollback on destruction
- **Connection pool** — lazy creation, checkout timeout; a global
  single-threaded maintainer runs heartbeats and reclaims idle connections
- **Dialect adaptation** — identifier quoting and paging syntax inferred from
  `SQL_DBMS_NAME` (backticks + LIMIT/OFFSET for MySQL/MariaDB, ANSI
  otherwise)
- **Code generation** — `uniorm-gen` connects to a live database, extracts
  the schema through ODBC metadata, and generates entity structs plus
  registration functions (TOML overrides for types/class names/skipped
  tables)

## Requirements

- A C++20 compiler (GCC 11+ / Clang 14+)
- CMake ≥ 3.20
- unixODBC (`find_package(ODBC)`) and an ODBC driver for the target database

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

Options:

| Option | Default | Description |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | Debug / Release / RelWithDebInfo / MinSizeRel |
| `UNIORM_DECIMAL_DEFAULT` | `string` | Default C++ mapping for DECIMAL/NUMERIC (`string` or `double`) |
| `UNIORM_BUILD_TESTS` | `ON` | Build unit/integration/perf tests |
| `UNIORM_BUILD_TOOLS` | `ON` | Build tools (`uniorm-gen`) |

The product is a shared library, `libuniorm.so` (headers in
`include/uniorm/`).

## Quick start

### Connection and raw SQL

```cpp
#include <uniorm/connection.hpp>

uniorm::connection conn("DSN=mydb;UID=user;PWD=secret");

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

### Aggregate projection (zero registration)

```cpp
struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

auto rows = conn.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", uniorm::params{18});
```

### Entity mapping + query builder

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

registry.validate(conn);  // reconcile against the live schema (optional)

using namespace uniorm;
auto adults = conn.query(registry)
                  .of<User>()
                  .where(gt(&User::age, 18) && like(&User::name, "a%"))
                  .order_by(&User::id, direction::desc)
                  .limit(10)
                  .all();  // direct binding onto User fields
```

### Batch insert

```cpp
std::vector<User> users = /* ... */;
std::size_t n = conn.insert(registry, users);  // NULLs, chunking, transaction

// Dynamic variant without an entity mapping
conn.insert_batch("users", {"name", "age"},
                  {uniorm::params{"alice", 30}, uniorm::params{"bob", nullptr}});
```

### Transactions and connection pool

```cpp
{
    auto txn = conn.begin();
    conn.execute_update("INSERT INTO logs (msg) VALUES (?)",
                        uniorm::params{"x"});
    txn.commit();  // no commit -> rollback on destruction
}

uniorm::pool_options opts;
opts.connection_string = "DSN=mydb;UID=user;PWD=secret";
opts.size = 8;
uniorm::connection_pool pool(std::move(opts));

{
    auto c = pool.acquire();      // throws pool_timeout on timeout
    c->execute_update("...");
}  // returned to the pool on destruction
```

The pool ships with a global single-threaded maintainer: it periodically
heartbeats idle connections (default `SELECT 1`), drops connections whose
heartbeat fails, and fully releases connections idle beyond `max_idle_time`
(default 10 minutes).

### Code generation (uniorm-gen)

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
```

Writes `build/gen/<name>_schema.hpp`: one struct per table (nullable columns
become `std::optional`), PK/FK/index metadata as comments, and a
`register_<name>_schema(uniorm::orm&)` function; `<name>` defaults to the
database name. The TOML config supports global and per-column C++ type
overrides, class renames, and skipped tables:

```toml
[types]                        # global SQL type -> C++ type overrides
"NUMERIC(10,2)" = "std::int64_t"

[tables.t_user]
class = "User"                 # class name override
skip = false

[tables.t_user.columns.status]
cpp_type = "std::string"       # per-column type override
```

Overrides are restricted to types the registry can bind; see design doc §6
for the full specification.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**: pure in-memory tests, no external dependencies
- **integration_tests**: needs a reachable ODBC DSN; reads `UNIORM_IT_DSN`,
  `UNIORM_IT_USER`, `UNIORM_IT_PWD` (all required); credentials
  are folded into the connection string as `UID`/`PWD`; ctest SKIPs when
  any of them is unset or the database is unreachable
- **perf_tests** (ctest label `perf`): batch-insert throughput and a
  comparison of the three query materialization paths (direct entity
  binding / aggregate projection / dynamic rows), with a raw-ODBC baseline
  that mirrors uniorm's exact call patterns as an abstraction-overhead
  reference; row count via `UNIORM_PERF_ROWS` (default 10000); skip with
  `ctest -LE perf`
- **gen_e2e_tests**: `uniorm-gen` end-to-end — the generated output is
  byte-compared against a checked-in golden header, which is itself
  compiled, registered, and `validate(strict)`ed; needs a reachable DSN

## Directory layout

```
include/uniorm/       public headers
  odbc/               RAII wrappers for ODBC handles (environment/connection/statement)
  detail/             pfr-lite, projection bindings, parameter staging, chrono helpers
  mapping/            entity mapping registry
  query/              predicate expressions and the query builder
src/                  implementation (built into libuniorm.so)
tools/uniorm-gen      code-generation CLI (schema extraction + TOML config + generator)
tests/unit            unit tests
tests/integration     database integration tests (+ golden header for uniorm-gen)
tests/perf            performance benchmarks
docs/design.md        design document (authoritative API reference)
```

## Status

v1 is complete and verified against MariaDB, including the `uniorm-gen`
end-to-end flow. v2 roadmap: backend abstraction (native libpq / Oracle OCI
channels), array-binding batch operations, and more — see design doc §9.
