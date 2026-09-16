# uniorm

[中文文档](README.zh.md)

A modern C++20 database access layer. Its core is driver-neutral: the scheme in
a connection string selects a backend, and the only backend built so far is
ODBC — which is what lets uniorm reach any database that ships an ODBC driver
without depending on vendor-specific C clients. Entity mapping, a
member-pointer query builder, transactions, and connection pooling sit on top
of that core.

See [docs/design.md](docs/design.md) for the full design.

## Features

- **Synchronous API with an exception-based error hierarchy** — every failure
  throws from the `uniorm_error` tree (`column_not_found`, `type_mismatch`,
  `mapping_error`, `pool_timeout`); a driver failure arrives as
  `backend::backend_error` carrying the SQLSTATE diagnostics, and a read no
  backend can serve as `backend::capability_not_supported`
- **UTF-8 everywhere internally** — string binding is narrow (`SQL_C_CHAR`);
  conversion from whatever encoding the driver speaks stays in the driver
- **Prepared statements + bind variables** — user values always go through
  `SQLBindParameter`; no string interpolation, no injection
- **Transparent statement cache** — an LRU cache keyed by SQL text skips
  re-prepare on repeated execution
  (observability: `statement_cache_hits()/misses()/statement_cache_size()`,
  and `clear_statement_cache()` to drop what is cached)
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
  through entity fields, parameters and projections; `validate()`'s strict mode
  compares it against the live column type
- **Exact decimals** — `uniorm::decimal_t` holds a fixed-point value (digits
  plus scale) that compares by value and allocates nothing per value;
  DECIMAL/NUMERIC columns default to a lossless `std::string` member, and a
  per-column `cpp_type` override swaps in `decimal_t`, which binds as exactly
  that string representation
- **Batch writes** — `db.insert(rows)` and batch `db.update(rows)` /
  `db.remove(rows)`: one row of placeholders sent with array parameter binding
  (`SQL_ATTR_PARAMSET_SIZE`), chunked by `paramset_size`, and wrapped in a
  transaction when the connection autocommits — in manual mode the chunks join
  the caller's
- **RAII transactions** — automatic rollback on destruction
- **Connection pool** — lazy creation, checkout timeout; a global
  single-threaded maintainer runs heartbeats and reclaims idle connections
- **Live schema introspection** — `orm::schema()` and `connection::schema()`
  hand out a `uniorm::schema_meta`: the database name, the tables, and per
  table its columns, primary key, foreign keys and indexes. `shape()` reduces a
  table to the `column_shape` list `validate()` checks mappings against, so the
  generator and the validator read through the same contract
- **Dialect adaptation** — identifier quoting and paging syntax inferred from
  `SQL_DBMS_NAME` (backticks + LIMIT/OFFSET for MySQL/MariaDB, ANSI
  otherwise)
- **Code generation** — `uniorm-gen` connects to a live database, reads
  its schema through the backend's introspection, and generates entity
  structs plus registration functions (TOML overrides for types/class
  names/skipped tables)
- **Pluggable backends** — the core API sits on a driver-neutral backend
  interface; the connection-string scheme selects the backend
  (`odbc://...`, or a bare ODBC connection string for backward
  compatibility), and ODBC is the only backend built so far. Catalog reads are
  part of that interface (`schema()`), so a backend without a catalog answers
  `capability_not_supported` rather than failing inside a read; capabilities
  are declared per backend, and two are consulted today: `columnar_batch`
  selects the bulk write path, `array_rowcount_totals` the batching of a
  row-counting sweep

## Requirements

- A C++20 compiler — the build asks for nothing but `cxx_std_20` and encodes no
  version floor; CI compiles it with Ubuntu 24.04's default GCC
- CMake ≥ 3.20
- unixODBC (`find_package(ODBC)`) and an ODBC driver for the target database,
  both only while `UNIORM_BACKEND_ODBC=ON`

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
| `UNIORM_BACKEND_ODBC` | `ON` | Build the ODBC backend into `libuniorm`; off leaves a core-only library with no backend, and `UNIORM_BUILD_TOOLS=ON` is then a configure error |

The product is a shared library, `libuniorm.so` (headers in
`include/uniorm/`): the target is `SHARED` unconditionally, so `BUILD_SHARED_LIBS`
changes nothing. Its `VERSION` is the project's (0.2.0) and its `SOVERSION` is
major.minor (0.2), on purpose — a consumer left holding a stale binary fails at
link time rather than at run time.

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
// are treated as ODBC connection strings (which needs the ODBC backend
// compiled in — a build with it off throws unknown_scheme).
//
// The string is also a pool key: this constructor leases its connection
// from a process-wide pool keyed on DSN and UID, with default
// pool_options. Pass a connection_pool of your own to choose the size,
// timeouts and maintainer instead.

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

db.validate();  // against the live schema; strict by default: a table or
                // column that is not there, a type the member cannot bind,
                // or a nullable column behind a non-optional member throws
// db.validate(uniorm::validation_mode::lenient);  // existence only

using namespace uniorm;
auto adults = db.query()
                .of<User>()
                .where(gt(&User::age, 18) && like(&User::name, "a%"))
                .order_by(&User::id, direction::desc)
                .limit(10)
                .all();  // direct binding onto User fields
```

### Schema introspection

```cpp
uniorm::schema_meta& md = db.schema();  // capability_not_supported if none

for (auto const& tbl : md.tables("", "public")) {  // "" = not restricted
    uniorm::schema_meta::table_ref ref{tbl.catalog, tbl.schema, tbl.name};
    for (auto const& c : md.table_columns(ref)) {
        // c.shape.name, c.shape.type, c.shape.nullable — what a mapping is
        // checked against — plus c.type_name, c.size, c.decimals and
        // c.default_value, which only a live read can carry
    }
    auto keys = md.primary_key(ref);      // column names, key order
    auto fks  = md.foreign_keys(ref);     // one row per column pair
    auto idx  = md.indexes(ref);          // name, columns, unique
}

// The one read validate() makes, reduced to the checked subset:
uniorm::table_shape shape = md.shape({{}, {}, "users"});
if (auto const* id = uniorm::find_column(shape, "id")) {
    bool nullable = id->nullable;
}
```

A `table_ref` with an empty catalog and schema asks for the name wherever the
connection can see it, so a table name that exists in two schemas at once comes
back as one merged shape. Name the schema in `ref` when that matters. Take the
reference afresh rather than caching it: it belongs to the leased connection, and
a `disconnect()` or a re-lease moves it.

### Batch writes

```cpp
std::vector<User> users = /* ... */;
std::size_t n = db.insert(users);   // NULLs and chunking by paramset_size
                                    // handled; one transaction when the
                                    // connection autocommits
std::size_t u = db.update(users);   // matched on the whole primary key
std::size_t r = db.remove(users);   // same; pass where_fields to override

// Single rows take the same overload set without the vector. The fields to
// match on are column names, and they apply to the batch forms too.
std::size_t one = db.remove(users.front(), { "name" });  // name alone

// Writes to a table with no entity mapping go through the dynamic builders.
// There is no dynamic insert: a table nobody maps has no column set to take.
std::size_t m = db.update("users")
                  .set("age", 31)
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .execute();
std::size_t d = db.remove("users")
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .execute();
```

Two sizes are settable on the `orm` and clamped if you hand them a zero:
`paramset_size` (default 1000) rows per bound execute, and `row_array_size`
(default 100) rows per fetch on every read path.

### Transactions and connection pool

```cpp
{
    auto txn = db.begin();
    db.execute_update("INSERT INTO logs (msg) VALUES (?)",
                      uniorm::params{"x"});
    txn.commit();  // no commit -> rollback on destruction
}

db.auto_commit(false);            // the connection's autocommit attribute:
db.execute_update("DELETE FROM logs", uniorm::params{});
                                  // nothing is durable until...
db.commit();                      // the caller commits, batch writes included

uniorm::pool_options opts;
opts.connection_string = "DSN=mydb;UID=user;PWD=secret";
opts.size = 8;                    // connections the pool may hold
opts.acquire_timeout = std::chrono::seconds{ 5 };
opts.heartbeat_interval = std::chrono::seconds{ 30 };  // 0 = no maintainer
opts.max_idle_time = std::chrono::minutes{ 10 };
opts.heartbeat_sql = "SELECT 1";
uniorm::connection_pool pool(opts);  // connections are made lazily, on demand

{
    uniorm::orm leased(pool);       // acquires now, returns on destruction;
    leased.execute_update("...");   // throws pool_timeout if none is free
}

auto slot = pool.acquire();         // the same lease without the orm layer:
                                    // a move-only handle that returns the
                                    // connection when it dies
std::size_t held = pool.capacity(); // also idle_count() and
                                    // heartbeats_executed()
```

Every pool shares **one** background scheduler thread, started for a pool whose
`heartbeat_interval` is nonzero: it heartbeats idle connections, drops any whose
heartbeat fails, and releases those idle beyond `max_idle_time` — which
`acquire()` also enforces on what it hands out. A connection coming back is reset
first (pending work rolled back, autocommit restored); if that fails the pool
retires it instead of handing out a dirty handle. The pool must outlive every
connection still checked out from it.

`orm("DSN=…")` needs no pool of yours: it leases from a process-wide registry of
pools keyed on DSN and UID, each built with default `pool_options`. Call
`connection_pool_registry::configure()` for that connection string to change
them.

### Code generation (uniorm-gen)

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
```

`--dsn` and `--connection-string` are two spellings of one thing — the first
assembles `DSN=<dsn>[;UID=…][;PWD=…]` and hands it to the same `uniorm::orm`
constructor you would use yourself — and exactly one of them is required, as is
`--out`. `--catalog` and `--schema` narrow where tables are looked for (empty
leaves that to the connection), `--name` renames the output unit, `--tables`
takes a comma-separated list.

Writes `build/gen/<name>_schema.hpp`: one struct per table (nullable columns
become `std::optional`, DECIMAL/NUMERIC arrive as a lossless `std::string`),
PK/FK/index metadata as comments, and a
`register_<name>_schema(uniorm::orm&)` function; `<name>` defaults to the
database name, or `db` when nothing in it is nameable. The TOML config supports
global and per-column C++ type overrides, class renames, and skipped tables:

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
  public API fails to compile. With `UNIORM_BUILD_TOOLS=ON` they also carry the
  generator's config and output tests, which name no driver any more and so live
  in the target that links none
- **odbc_unit_tests**: the private ODBC handle layer and its error translation
  (driver manager only, no DSN required); built only with
  `UNIORM_BACKEND_ODBC=ON`
- **integration_tests**: needs a reachable ODBC DSN; reads `UNIORM_IT_DSN`,
  `UNIORM_IT_USER`, `UNIORM_IT_PWD` (all required); credentials
  are folded into the connection string as `UID`/`PWD`; ctest SKIPs when
  any of them is unset or the database is unreachable
- **perf_tests** (ctest label `perf`): batch-insert throughput and a
  comparison of the three query materialization paths (direct entity
  binding / aggregate projection / dynamic rows), with a raw-ODBC baseline
  that mirrors uniorm's exact call patterns as an abstraction-overhead
  reference; row count via `UNIORM_PERF_ROWS`, which falls back to 10000 when
  unset, zero, or above a million; skip with `ctest -LE perf`
- **gen_e2e_tests**: `uniorm-gen` end-to-end — the tool's generated code is
  compared with a checked-in golden header, comments excluded, and that header
  is itself compiled, registered and `validate(strict)`ed; needs a reachable DSN
- **install_smoke**: packaging check — `cmake --install` into a throwaway prefix
  under the build dir, then an outside project (`tests/install/`) configures,
  builds and runs against it through `find_package(uniorm CONFIG)` without setting
  a C++ standard of its own. It also checks that the package files landed, that
  the installed header set equals `include/uniorm/` (so an install rule that
  starts filtering headers shows up), that asking `find_package` for `99.0.0` is
  turned away by the `SameMinorVersion` gate, and — when `UNIORM_BUILD_TOOLS=ON`
  — that the installed `uniorm-gen` starts, which is where a broken
  prefix-relative RPATH would surface. Runs for any top-level,
  non-cross-compiling build, with or without the ODBC backend; the last check
  only when tools are on

The three DSN-reading targets exist only while `UNIORM_BACKEND_ODBC=ON`, and
`gen_e2e_tests` only while `UNIORM_BUILD_TOOLS=ON` too.

## Directory layout

```
include/uniorm/       public headers (the only include root consumers need)
  backend/            driver-neutral backend interface, registry, errors
  detail/             pfr-lite, projection bindings, chrono helpers
  mapping/            entity mapping registry
  builder/            predicate expressions and the fluent query/update/delete builders
  schema.hpp          the introspection contract every backend answers to
src/                  implementation (built into libuniorm.so); private headers
                        sit beside their .cpp and are reached by relative name
  backend/            scheme parsing and the backend registry
  odbc/               ODBC backend: adapter, handle wrappers, errors, and the
                        catalog reads that answer schema()
                        (the only <sql.h> in the tree)
  statement_cache.hpp prepared-statement LRU cache
  orm_mapping.hpp     what an entity write matches on and assigns, resolved
                        from the mapping alone — no connection involved
cmake/                the package config template the installed find_package reads
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
through Connector/ODBC with server-side prepares, and against PostgreSQL 17
through psqlODBC. v2 is underway: the
backend abstraction is in place (neutral interface + scheme-based registry,
ODBC migrated behind it, ODBC linked privately, core unit tests compile and
run without ODBC), v1's last type-level debt is closed
(`uniorm::decimal_t`), and the schema an introspecting caller reads is now a
public contract of its own: `orm::schema()` hands out a `uniorm::schema_meta`
and the catalog reads sit behind it, in the backend that owns them. The
generator builds through the same `orm`/`schema()` pair a consumer would, so it
names no driver and links nothing below what `find_package` hands over — which
is also why its config and output tests moved to the target that links no ODBC.
A CI workflow now guards both shapes — the ODBC-free
compile contract, and the suite against a live server for each connector
paired with the server it is used against. The server axis is there because
the generator reads metadata the *server* answers, not SQL the dialect
writes: against MySQL 8.4, MariaDB connector 3.1.12 asks
`information_schema` for `COLUMN_KEY = 'pri'`, matches nothing once that
server declares those columns `utf8mb3_bin`, and generates a header whose
primary key is gone — one that still compiles, registers and validates. That
class of silence is caught by the comparison itself, which every leg runs:
generated code against the golden's, comments cut from both sides, since those
comments differ per connector-and-server pair and comparing them would nail
the golden to one cell. The lost key is `.column` where the golden has
`.primary_key`; markers are left for the foreign key and the secondary index,
the two facts no line of generated code carries. GitHub has run a two-leg
shape green — both legs on a MariaDB server then — after one execution caught
a driver header reaching the ODBC-free build, which no local replay could
have, since the local image had the ODBC development headers installed to
build the connectors with. Those legs then took both servers and passed all
five tests in all four cells inside an `ubuntu:24.04` container carrying both
pinned connectors, and came back down to the two pairings a driver is used
with: the cross cells are out by choice, and design doc §9 keeps that as a
named gap. A third leg has since grown on — psqlODBC against PostgreSQL 17, the
only one of the three drivers Ubuntu packages — and it is what found the single
place where a *driver's* answer, not the server's, used to decide a return
value: psqlODBC applies every parameter set of an array-bound UPDATE or DELETE
but leaves `SQLRowCount` at one set's count, where both MySQL-wire connectors
report the array's total. `update()` and `remove()` return that number, so the
core asks for `array_rowcount_totals` and sweeps one set per execute without
it. One golden now serves all three families, because the fixture tables are
spelled the way all of them parse. Each leg still asks which server answered
before it builds, and stops if that is not the one its name claims — on
PostgreSQL the question is `SHOW SERVER_VERSION`, since `SELECT VERSION()`
there starts with the server's name rather than a number. That four-job shape
is rehearsed step for step in the runner's own image family and still owed a
runner pass. Native libpq / Oracle OCI backends follow — see design doc §5 and
§9.
