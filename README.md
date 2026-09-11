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
- **UTF-8 everywhere internally** — string binding is narrow (`SQL_C_CHAR`);
  conversion from whatever encoding the driver speaks stays in the driver
- **Prepared statements + bind variables** — user values always go through
  `SQLBindParameter`; no string interpolation, no injection
- **Transparent statement cache** — an LRU cache keyed by SQL text skips
  re-prepare on repeated execution
  (observability: `statement_cache_hits()/misses()/statement_cache_size()`)
- **Three access levels**:
  - Raw SQL: `execute` / `execute_update` with `params`
  - Aggregate projection: `db.query<Row>(sql)` maps columns onto a plain
    struct with zero registration
  - Entity mapping: explicit registry plus a type-safe member-pointer query
    builder, `db.query().of<T>()`
- **Direct entity binding** — `query<T>::all()/one()` bind result columns
  straight onto entity fields (`SQLBindCol`), bypassing row materialization
- **Custom type mapping** — specializing `uniorm::converter<T>` names the SQL
  representation a domain type binds as, and that representation carries it
  through entity fields, parameters and projections; `validate(strict)`
  compares it against the live column type
- **Exact decimals** — `uniorm::decimal_t` holds a fixed-point value (digits
  plus scale) that compares by value and allocates nothing per value;
  DECIMAL/NUMERIC columns default to a lossless `std::string` member, and a
  per-column `cpp_type` override swaps in `decimal_t`, which binds as exactly
  that string representation
- **Batch writes** — `db.insert(rows)` and batch `db.update(rows)` /
  `db.remove(rows)`: one row of placeholders sent with array parameter binding
  (`SQL_ATTR_PARAMSET_SIZE`), chunked by `paramset_size`, wrapped in a
  transaction
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
- **Pluggable backends** — the core API sits on a driver-neutral backend
  interface; the connection-string scheme selects the backend
  (`odbc://...`, or a bare ODBC connection string for backward
  compatibility); capabilities are declared per backend, but only
  `columnar_batch` is consulted today

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
| `UNIORM_BUILD_TESTS` | `ON` | Build unit/integration/perf tests |
| `UNIORM_BUILD_TOOLS` | `ON` | Build tools (`uniorm-gen`) |
| `UNIORM_BACKEND_ODBC` | `ON` | Build the ODBC backend into `libuniorm`; off builds are core-only (`UNIORM_BUILD_TOOLS` must be off too) |

The product is a shared library, `libuniorm.so` (headers in
`include/uniorm/`).

### Installing

```sh
cmake --install build --prefix /path/to/prefix
```

That puts the versioned library (plus its `SOVERSION` symlinks; on Windows the
DLL and its import library) in `<libdir>/`, every public header in
`<prefix>/include/uniorm/`, a package config in `<libdir>/cmake/uniorm/`, and
the `uniorm-gen` CLI in `<bindir>/`. A consumer needs nothing from this
repository:

```cmake
find_package(uniorm REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE uniorm::uniorm)
```

The C++20 requirement travels with the target, so a consumer needs no
`CMAKE_CXX_STANDARD` of its own.

Source integration is unchanged: `add_subdirectory` or FetchContent hand over
the same `uniorm::uniorm` target and install nothing. The install rules only
apply when uniorm is the top-level project. The CLI installs only with
`UNIORM_BUILD_TOOLS=ON`, which in turn requires the ODBC backend; `find_package`
still hands over `uniorm::uniorm` alone, since the CLI is a program to run, not
a target to link.

## Quick start

### Connection and raw SQL

```cpp
#include <uniorm/uniorm.hpp>

uniorm::orm db("DSN=mydb;UID=user;PWD=secret");
// Equivalent explicit form: "odbc://DSN=mydb;UID=user;PWD=secret".
// The scheme before :// selects the backend; strings without a scheme
// are treated as ODBC connection strings.

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

### Aggregate projection (zero registration)

```cpp
struct user_row {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

auto rows = db.query<user_row>(
    "SELECT id, name, age FROM users WHERE age > ?", uniorm::params{18});
```

### Entity mapping + query builder

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

db.validate();  // reconcile against the live schema (optional)

using namespace uniorm;
auto adults = db.query()
                .of<User>()
                .where(gt(&User::age, 18) && like(&User::name, "a%"))
                .order_by(&User::id, direction::desc)
                .limit(10)
                .all();  // direct binding onto User fields
```

### Batch writes

```cpp
std::vector<User> users = /* ... */;
std::size_t n = db.insert(users);   // NULLs, chunking, one transaction
std::size_t u = db.update(users);   // matched on the whole primary key
std::size_t r = db.remove(users);   // same; pass where_fields to override

// Writes without an entity mapping go through the dynamic builders
std::size_t m = db.update("users")
                  .set("age", 31)
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .execute();
```

### Transactions and connection pool

```cpp
{
    auto txn = db.begin();
    db.execute_update("INSERT INTO logs (msg) VALUES (?)",
                      uniorm::params{"x"});
    txn.commit();  // no commit -> rollback on destruction
}

db.auto_commit(false);            // the connection's autocommit attribute:
db.execute_update("...", p);      // nothing is durable until...
db.commit();                      // the caller commits, batch writes included

uniorm::pool_options opts;
opts.connection_string = "DSN=mydb;UID=user;PWD=secret";
opts.size = 8;
uniorm::connection_pool pool(std::move(opts));

{
    uniorm::orm leased(pool);       // acquires now, returns on destruction;
    leased.execute_update("...");   // throws pool_timeout if none is free
}
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

- **unit_tests**: pure in-memory tests, no external dependencies; they run
  against a fake backend and link no ODBC, so any driver-type leak into the
  public API fails to compile
- **odbc_unit_tests**: ODBC handle and `uniorm-gen` unit tests
  (driver manager only, no DSN required)
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
- **gen_e2e_tests**: `uniorm-gen` end-to-end — the tool's generated code is
  compared with a checked-in golden header, comments excluded, and that header
  is itself compiled, registered and `validate(strict)`ed; needs a reachable DSN
- **install_smoke**: packaging check — `cmake --install` into a throwaway prefix
  under the build dir, then an outside project (`tests/install/`) configures,
  builds and runs against it through `find_package(uniorm CONFIG)` without setting
  a C++ standard of its own; it also asserts the package version gate turns away
  another minor and that the installed `uniorm-gen` starts through its
  `$ORIGIN`-relative RPATH. No DSN required

## Directory layout

```
include/uniorm/       public headers (the only include root consumers need)
  backend/            driver-neutral backend interface, registry, errors
  detail/             pfr-lite, projection bindings, chrono helpers
  mapping/            entity mapping registry
  builder/            predicate expressions and the fluent query/update/delete builders
src/                  implementation (built into libuniorm.so); private headers
                        sit beside their .cpp and are reached by relative name
  backend/            scheme parsing and the backend registry
  odbc/               ODBC backend: adapter, handle wrappers, errors
                        (the only <sql.h> in the tree)
  statement_cache.hpp prepared-statement LRU cache
tools/uniorm-gen      code-generation CLI (schema extraction + TOML config + generator)
tests/unit            unit tests
tests/integration     database integration tests (+ golden header for uniorm-gen)
tests/install         external consumer project (drives the install_smoke check)
tests/perf            performance benchmarks
docs/design.md        design document (authoritative API reference)
```

## Status

v1 is complete and verified against MariaDB, including the `uniorm-gen`
end-to-end flow; the same suite also passes against a real MySQL server
through Connector/ODBC with server-side prepares. v2 is underway: the
backend abstraction is in place (neutral interface + scheme-based registry,
ODBC migrated behind it, ODBC linked privately, core unit tests compile and
run without ODBC), and v1's last type-level debt is closed
(`uniorm::decimal_t`). A CI workflow now guards both shapes — the ODBC-free
compile contract, and the suite against a live server for each combination of
the two connectors with the two servers on that wire. The second axis is
there because the generator reads metadata the *server* answers, not SQL the
dialect writes: against MySQL 8.4, MariaDB connector 3.1.12 asks
`information_schema` for `COLUMN_KEY = 'pri'`, matches nothing once that
server declares those columns `utf8mb3_bin`, and generates a header whose
primary key is gone — one that still compiles, registers and validates. That
class of silence is now caught by the comparison itself: every leg matches
the generated code against the golden's with the comments cut from both
sides, since those comments differ per connector-and-server pair and
comparing them would nail the golden to one cell. The lost key is `.column`
where the golden has `.primary_key`; markers are left for the foreign key and
the secondary index, the two facts no line of generated code carries. GitHub
has run the two-leg shape green, after one execution caught a driver header
reaching the ODBC-free build — which no local replay could have, since the
local image had the ODBC development headers installed to build the
connectors with. The four-leg shape is still ahead of it, though a real MySQL
8.4 server has already seen a local preflight: four of the five green, the
fifth red only through the older connector this machine has. Native libpq /
Oracle OCI backends follow — see design doc §5 and §9.
