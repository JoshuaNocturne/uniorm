# uniorm

[中文文档](README.zh.md)

A C++20 database access layer with a driver-neutral core: the scheme in a
connection string selects the backend, and the API itself names no vendor's
client. Three backends ship: ODBC, built by default, with Oracle OCI and
libpq behind build options. Entity mapping, a member-pointer query builder,
transactions, pooling, and async sit on top of that core.

The full API reference is [docs/design.md](docs/design.md).

## Features

- **Exceptions** — every failure throws from the `uniorm_error` tree
  (`column_not_found`, `type_mismatch`, `mapping_error`, `pool_timeout`); a
  backend failure arrives as `backend::backend_error` carrying the native
  diagnostics (`SQLSTATE` and message on ODBC), and a read no backend can
  serve as `backend::capability_not_supported`
- **UTF-8 internally** — strings travel as narrow bytes (`SQL_C_CHAR` on
  ODBC); conversion from whatever encoding the server speaks stays in the
  backend
- **Prepared statements + bind variables** — user values always go through the
  backend's bind call (`SQLBindParameter` on ODBC), never string
  interpolation
- **Statement cache** — LRU keyed by SQL text; `statement_cache_hits()`,
  `statement_cache_misses()`, `statement_cache_size()`,
  `clear_statement_cache()`
- **Three access levels** — raw SQL (`execute` / `execute_update` with
  `params`), aggregate projection (`db.query<Row>(sql)`, zero registration),
  entity mapping (`db.query<T>()`)
- **Direct entity binding** — `query<T>::all()/one()` bind result columns
  straight onto entity fields (`SQLBindCol` on ODBC), skipping row
  materialization
- **Custom type mapping** — specializing `uniorm::converter<T>` names the SQL
  representation a domain type binds as; strict `validate()` compares it
  against the live column type
- **Exact decimals** — `uniorm::decimal_t` holds a fixed-point value (digits
  plus scale), compares by value, allocates nothing per value; DECIMAL/NUMERIC
  columns default to a lossless `std::string` member and a per-column
  `cpp_type` override swaps in `decimal_t`
- **Batch writes** — `db.insert(rows)` and batch `update` / `remove`: one row
  of placeholders sent as an array of parameter sets
  (`SQL_ATTR_PARAMSET_SIZE` on ODBC), chunked by `paramset_size`, wrapped in a
  transaction when the connection autocommits
- **RAII transactions** — automatic rollback on destruction
- **Async** (`async.hpp`) — the same synchronous calls as
  `uniorm::async::task<T>`: one session runs one operation at a time, three
  drive actions (`co_await`, `block()`, `start()`), and the four CRUD names also
  take a plain callback
- **Connection pool** — lazy creation, checkout timeout, one shared
  single-threaded maintainer for heartbeats and idle reclamation
- **Schema introspection** — `orm::schema()` and `connection::schema()` hand
  out a `uniorm::schema_meta`: the database name, the tables, and per table its
  columns, primary key, foreign keys and indexes
- **Dialect adaptation** — identifier quoting and paging syntax inferred from
  what the server reports about itself (`SQL_DBMS_NAME` on ODBC: backticks +
  LIMIT/OFFSET for MySQL/MariaDB, ANSI otherwise), plus an identifier spelling
  policy: `keep` (default), `lower` or `upper`
- **Code generation** — `uniorm-gen` reads a live schema, or a DDL file, and
  generates entity structs plus registration functions (TOML overrides for
  types, class names, skipped tables)
- **Pluggable backends** — the connection-string scheme selects the backend
  (`odbc://...`, `oracle://...`, `postgresql://...`, or a bare ODBC string for
  compatibility). A `connection_spec` — scheme, target, user, password — is the
  alternative spelling, handed to that backend unfilled so it writes its own
  dialect's words. Catalog reads are part of the same interface.

## Requirements

- A C++20 compiler; the build asks for nothing but `cxx_std_20`
- CMake ≥ 3.21
- unixODBC (`find_package(ODBC)`) and an ODBC driver for the target database,
  only while `UNIORM_BACKEND_ODBC=ON`
- Oracle Call Interface (an Instant Client, or a full client's SDK), only while
  `UNIORM_BACKEND_ORACLE_OCI=ON`; `OCI_ROOT` or `ORACLE_HOME` points at the tree
- libpq (`find_package(PostgreSQL)`), only while `UNIORM_BACKEND_LIBPQ=ON`

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

| Option | Default | Description |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | Debug / Release / RelWithDebInfo / MinSizeRel |
| `UNIORM_BUILD_TESTS` | `ON` | Build unit/integration/perf tests |
| `UNIORM_BUILD_TOOLS` | `ON` | Build tools (`uniorm-gen`) |
| `UNIORM_BACKEND_ODBC` | `ON` | Build the ODBC backend plugin (`libuniorm_odbc.so`); off leaves a core-only library with no backend, and `UNIORM_BUILD_TOOLS=ON` is then a configure error |
| `UNIORM_BACKEND_ORACLE_OCI` | `OFF` | Build the Oracle OCI backend (target `uniorm_oracle_oci`, loaded as `libuniorm_oracle`); needs Oracle Call Interface on the machine configuring it |
| `UNIORM_BACKEND_LIBPQ` | `OFF` | Build the PostgreSQL backend (target `uniorm_libpq`, loaded as `libuniorm_postgresql`); needs libpq on the machine configuring it |

The product is a shared library, `libuniorm.so` (headers in
`include/uniorm/`), `VERSION` 0.4.0 and `SOVERSION` 0.4. Each enabled backend
is a library of its own, `libuniorm_<scheme>.so`, and the core links no driver.
The SONAME and the plugin ABI gate move together on `major.minor`, so a plugin
built against another minor is refused at load — rebuild consumers and
third-party backends with the core rather than mixing binaries.

### Installing

```sh
cmake --install build --prefix /path/to/prefix
```

That puts the versioned library (plus its `SOVERSION` symlinks; on Windows the
DLL and its import library) in `<libdir>/`, each enabled backend in
`<libdir>/uniorm/backends/` (`<bindir>/uniorm/backends/` on Windows), every
public header in `<prefix>/include/uniorm/`, a package config in
`<libdir>/cmake/uniorm/`, and the `uniorm-gen` CLI in `<bindir>/`. A consumer
needs nothing else:

```cmake
find_package(uniorm REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE uniorm::uniorm)
```

The C++20 requirement travels with the target, so no `CMAKE_CXX_STANDARD` of
the consumer's own. Which backend loads is decided at run time by the scheme,
from that `uniorm/backends/` directory; `UNIORM_PLUGIN_PATH` overrides where
the loader looks. Source integration is unchanged: `add_subdirectory` or
FetchContent hands over the same target and installs nothing. The install rules
apply only when uniorm is the top-level project, and the CLI installs only with
`UNIORM_BUILD_TOOLS=ON`.

## Quick start

### Connection and raw SQL

```cpp
#include <uniorm/uniorm.hpp>

uniorm::orm db("DSN=mydb;UID=user;PWD=secret");
// Equivalent: "odbc://DSN=mydb;UID=user;PWD=secret". The scheme before ://
// selects the backend; a schemeless string is an ODBC connection string, so a
// build with the ODBC backend off throws unknown_scheme.
//
// The string is also a pool key: this constructor leases from a process-wide
// pool keyed on DSN and UID, with default pool_options. Pass a connection_pool
// of your own to choose the size, timeouts and maintainer instead.

// The same database, without writing one backend's string:
uniorm::orm pieces(uniorm::connection_spec{
    "odbc",                       // the only field uniorm itself reads
    "mydb",                       // where: a DSN name, a host:port/db, ...
    "user", "secret"});           // empty = no credential supplied

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

The `connection_spec` is handed to the backend that owns the scheme, because
only that backend knows how its pieces must be spelled: the ODBC one writes
`DSN=` / `UID=` / `PWD=` and braces a value that needs it.
`orm::connect(...)` takes the same shape. It claims no completeness — a
backend's own keyword, a port, a TLS policy all belong to the string form,
which stays the primitive every backend implements. A spec and a string that
reach the same server keep separate pools.

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
struct User {
    std::int64_t id;
    std::string name;
    std::optional<std::int32_t> age;
};

db.map<User>("users")
    .primary_key("id", &User::id)
    .column("name", &User::name)
    .column("age", &User::age);

db.validate();  // against the live schema; strict by default: a missing table
                // or column, a type the member cannot bind, or a nullable
                // column behind a non-optional member throws
// db.validate(uniorm::validation_mode::lenient);  // existence only

using namespace uniorm;
auto adults = db.query<User>()
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
        // c.shape.name / c.shape.type / c.shape.nullable — what a mapping is
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
connection can see it, so a name carried by two schemas comes back as one
merged shape; name the schema in `ref` when that matters. The reference belongs
to the leased connection — take it afresh rather than caching it across a
`disconnect()` or a re-lease.

### Identifier spelling

```cpp
db.identifier_case(uniorm::dialect::identifier_case::upper);  // default: unset
```

Left unset, the SQL follows the server's own dialect: Oracle folds unquoted
names to upper case and PostgreSQL folds them to lower, so neither needs a
declaration; every other server keeps declared names verbatim — declaring
`keep` is a decision of its own, not the absence of one. `lower` and `upper`
fold ASCII letters and nothing else. Set it once after connecting; the `orm`
re-applies it to every connection it leases. The fold runs wherever a backend
reaches that server, over ODBC as well as over libpq: a schema whose names were
created quoted and mixed-case is answered by `keep` or by resolution, not by the
default. `validate()` asks the catalog
under the same policy, and a miss names the spelling the catalog has:

    table not found: USER_ACCOUNTS (only case differs from 'user_accounts')

A fold emits the spelling it produces, not the one declared: where a schema
holds both `"Users"` and `"users"`, a declared `Users` reads the lower-cased
table under `lower`, and `validate()` calls that a hit. Naming the table that
was actually written takes resolution, below.

A single fold cannot reconcile every mapping — MariaDB may store tables
lowercase but preserve uppercase column spellings. Resolve each name explicitly
after registration instead:

```cpp
db.resolve_identifiers("app_db", "public");  // PostgreSQL
// db.resolve_identifiers("app_db", "");    // MySQL / MariaDB
db.validate();
```

Resolution selects an exact name first, then a unique ASCII-case variant; a
missing or ambiguous name throws `mapping_error`, and all mappings install
together or none change. Resolved mappings use the actual quoted names and
ignore `identifier_case`. PostgreSQL requires its current database and an
explicit schema; MySQL/MariaDB require a database and an empty schema. Only
ordinary tables are supported, and unsupported backends fail explicitly — the
libpq backend answers no catalog reads, so on that leg both `schema()` and
`validate()` raise.
`clear_identifier_resolution()` restores normal folding; connect and disconnect
clear results too. Hand-written SQL is never rewritten, and dynamic table
builders stay on the connection's spelling policy.

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

// Writes to a table with no entity mapping go through the dynamic builders
// (there is no dynamic insert: a table nobody maps has no column set to take).
// Chained where() calls accumulate and AND, matching query<T>::where.
std::size_t m = db.update("users")
                  .set("age", 31)
                  .where("id = ?", uniorm::params{ std::int64_t{ 2 } })
                  .where("name = ?", uniorm::params{ std::string("bob") })
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
opts.target = "DSN=mydb;UID=user;PWD=secret";
                                  // or the four pieces:
                                  // opts.target = uniorm::connection_spec{
                                  //     "odbc", "mydb", "user", "secret" };
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
`acquire()` also enforces on what it hands out. A connection coming back is
reset first; if that fails the pool retires it rather than handing out a dirty
handle. The pool must outlive every connection still checked out from it.

`orm("DSN=…")` needs no pool of yours: it leases from a process-wide registry
keyed on DSN and UID, each built with default `pool_options`. Call
`connection_pool_registry::configure()` for that connection string to change
them.

### Async

Nothing here is a second database path: a task body *is* the synchronous call
it wraps — `orm::execute_update`, the fetch loop, a whole batch sweep — on a
thread the caller does not wait on. ODBC connectors expose no native async
entry points, so this is emulation; a backend that does have them replaces the
layer with no caller changing a line.

```cpp
#include <uniorm/async.hpp>

uniorm::async::task<void> rename(uniorm::async::orm& db) {
    auto tx = co_await db.begin();
    co_await db.execute_update("UPDATE users SET name = ? WHERE id = ?",
                               uniorm::params{"Ada", 7});
    co_await tx.commit();   // dropping without it rolls back, and that blocks
}

int main() {
    uniorm::async::orm db;
    db.map<User>("users")                  // configuration is forwarded by name
        .primary_key("id", &User::id)
        .column("name", &User::name);
    db.connect("DSN=mydb;UID=user;PWD=secret").block();

    rename(db).block();                    // park this thread until it is over
    db.execute_update("DELETE FROM visits", uniorm::params{})
      .start([](std::exception_ptr bad) {  // hand it over and keep going
        if (bad) { /* the write is lost; this handler is the only trace */ }
      });
    auto names = db.query<User>().all().block();
}                                          // ~orm waits for handed-over work
```

What the shape guarantees:

- **The worker count is the ceiling.** `default_executor_size(n)` before the
  first task decides it; afterwards the pool only grows. Parallelism is more
  sessions, never more tasks on one.
- **A session runs one operation at a time.** `co_await`, `block()` and
  `start()` share one FIFO per session; a request waiting its turn holds no
  thread. Past `submit_limit()` (1024 to start with) the drive action throws
  `submit_overflow` and leaves the task undriven, so trying again later is
  legal. `pending()` counts the queue.
- **A failure has exactly one exit.** `start()` calls its `on_failure` for
  nothing else; `block()` and `co_await` rethrow the same `exception_ptr`.
- **A continuation changes threads by default.** `run_on(executor&)` says where
  blocking work goes, `resume_on(executor&)` where it comes back to; a one-off
  script names neither.
- **Four points block on purpose:** the catalog reads that stayed synchronous
  (`validate()`, `schema()`, `resolve_identifiers()`, reached through `sync()`),
  the rollback `~async::transaction` takes as a fallback, and the wait `~orm()`
  performs — `wait_all_operations()` is the same wait by name.

The four CRUD names also take a plain callback, for a caller who wants one call
and one answer:

```cpp
db.insert(users, [](std::size_t added, std::exception_ptr bad) {
    if (bad) { /* added is 0 here, not a partial count */ return; }
    std::printf("%zu rows in\n", added);
});

db.query<User>("SELECT id, name FROM users WHERE id > ?",
               uniorm::params{std::int64_t{9000}},
               [](std::vector<User> rows, std::exception_ptr bad) {
    if (bad) { return; }
    use(rows);
});
```

**Check the second argument first**: on failure the first is `R{}`. The callback
answers on the thread that ran the operation while that operation still holds
its turn, so a slow callback holds up the next request on the same session — and
that is why `~orm()` waits for every callback it owes, which makes capturing
locals by reference legal. Covers the four CRUD names only; the builder
terminals, `connect` and `begin` stay task-shaped.

### Code generation (uniorm-gen)

```sh
uniorm-gen --dsn=mydb --user=u --password=p \
           --config=uniorm.toml --out=build/gen [--tables=a,b]
uniorm-gen --input-ddl=schema.sql --out=build/gen [--name=app]
```

Exactly one of `--dsn`, `--connection-string`, `--input-ddl` is required, as
is `--out`. `--catalog` and `--schema` narrow where tables are looked for:
each name is matched by name, not as a wildcard pattern, so a `%` there
matches nothing (the run says so). `--name` renames the output unit and
`--tables` takes a comma-separated list.

`--input-ddl` reads a create-table script instead of a live server, so that run
opens no connection. It takes the MySQL and PostgreSQL spellings of
`CREATE TABLE` (columns and table-level keys), `CREATE INDEX` and the adding
forms of `ALTER TABLE`; anything it recognizes that carries no mapping (`DROP`,
`MODIFY`, storage clauses) is counted into one warning rather than failing the
run. `USE app;` supplies the default unit name.

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

Overrides are restricted to types the registry can bind. Config keys match
regardless of case; a section naming no table or column fails the run instead of
being ignored, and so do two tables that end up sharing one class name. See
design doc §6 for the full specification.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests** — in-memory tests over a fake backend and a fake catalog;
  links no backend library, so a driver type leaking into the public API fails
  to compile. Carries the generator's config and output tests too
- **odbc_unit_tests** — the private ODBC handle layer, its error translation,
  the catalog's name re-filter (driver manager only, no DSN required)
- **oracle_oci_unit_tests** — the placeholder rewrite, Oracle's type table,
  connect-string splitting (client library only, no server required)
- **libpq_unit_tests** — the placeholder rewrite, PostgreSQL's type table,
  connect-string splitting (client library only, no server required)
- **integration_tests** — needs a reachable ODBC DSN; reads `UNIORM_IT_DSN`,
  `UNIORM_IT_USER`, `UNIORM_IT_PWD` (all required), and ctest SKIPs when any is
  unset or the database is unreachable
- **integration_tests_oracle** — the same suite over the Oracle plugin, with
  `UNIORM_IT_BACKEND=oracle` and the target read as `host[:port]/service`
  instead of a DSN; its catalog stages report themselves skipped
- **integration_tests_postgresql** — the same suite over the libpq plugin, with
  `UNIORM_IT_BACKEND=postgresql` and the target read as `host[:port]/dbname`
  instead of a DSN; its catalog stages report themselves skipped, and the stage
  that drops a prepared statement reports itself skipped on the legs that do
  not hold one by name
- **perf_tests** (label `perf`) — the ODBC leg: batch writes and the three
  materialization paths beside a hand-called ODBC baseline paired with them
  case by case; row count via `UNIORM_PERF_ROWS` (default 10000); skip with
  `ctest -LE perf`
- **perf_tests_oracle** — the same uniorm cases over the Oracle plugin beside
  OCI called by hand; the shapes that plugin does not ask a client for (array
  DML, row prefetch) are reported on their own, where the client accepts an
  array bind at all, and the target is read as `host[:port]/service`
- **perf_tests_libpq** — the same uniorm cases over the libpq plugin beside
  libpq called by hand, where a batch is one prepared statement per row; the
  one statement carrying a thousand rows' VALUES lists stands on its own, and
  the target is read as `host[:port]/dbname`
- **gen_e2e_tests** — generated code compared with a checked-in golden header,
  which is itself compiled, registered and `validate(strict)`ed; needs a DSN
- **install_smoke** — `cmake --install` into a throwaway prefix, then an outside
  project (`tests/install/`) configures, builds and runs through
  `find_package(uniorm CONFIG)` without setting a C++ standard of its own

The three DSN-reading targets exist only while `UNIORM_BACKEND_ODBC=ON`, and
`gen_e2e_tests` only while `UNIORM_BUILD_TOOLS=ON` too.

## Directory layout

```
include/uniorm/       public headers (the only include root consumers need)
  backend/            driver-neutral backend interface, registry, errors
  detail/             pfr-lite, projection bindings, chrono helpers
  mapping/            entity mapping registry
  builder/            predicate expressions and the fluent builders
  schema.hpp          the introspection contract every backend answers to
  async.hpp           task<T>, executor, async::orm, async::transaction
                        (not in the umbrella header, so coroutines stay opt-in)
src/                  implementation (built into libuniorm.so)
  async/              the default worker pool, the session FIFO
  backend/            scheme parsing, the registry, the plugin loader
  statement_cache.hpp prepared-statement LRU cache
  orm_mapping.hpp     what an entity write matches on and assigns
backends/odbc/        ODBC backend (the only <sql.h> in the tree)
backends/oracle_oci/  Oracle OCI backend (the only <oci.h>; built on request)
backends/libpq/       libpq backend (the only <libpq-fe.h>; built on request)
cmake/                the package config template find_package reads
tools/uniorm-gen      code-generation CLI
tests/unit            unit tests
tests/integration     database integration tests (+ golden header)
tests/install         external consumer project
tests/perf            performance benchmarks
docs/design.md        design document (authoritative API reference)
```

## Status

v1 is complete and verified against a live database: the three access paths,
batch writes, transactions, the pool, the dialect layer, `uniorm::decimal_t`,
and the `uniorm-gen` end-to-end flow. The same suite passes on MariaDB, on MySQL
through Connector/ODBC with server-side prepares, and on PostgreSQL through
psqlODBC.

v2 is underway, with the pieces landed on top of v1: the driver-neutral backend
abstraction — each backend now a separate plugin the core loads by scheme at
run time — introspection as a public contract, identifier spelling as a
deployment policy, `connection_spec`, the async layer, and two more backends
(Oracle OCI, libpq) built on that same contract.

CI runs five job definitions, seven instances: `core`, `core-windows`,
`oracle-oci` and `libpq` stand alone, while `driver` carries three legs, each
pairing a connector with the server it is used against. Every instance also
compiles one translation unit per installed header, so a public header that
only parses behind a neighbour's include fails here instead of in a consumer's
project, and a leg whose tests skip fails, so a green leg means the server was
really reached. The six written down before the libpq leg are green on the
runner; that leg is green against a live container replayed by hand, and the
runner's own half of it — apt's libpq, the image pull, the health gate — is
unconfirmed.

Open: the native array binding in the OCI backend, and in both native backends
the catalog reads they have yet to answer — see design doc §5.4 and §9.

## License

MIT — see [LICENSE](LICENSE).
