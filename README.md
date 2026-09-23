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
  otherwise), plus a spelling policy the deployment declares: `keep` (default,
  verbatim), `lower` or `upper`, applied where identifiers reach SQL and by
  `validate()` when it asks the catalog
- **Code generation** — `uniorm-gen` connects to a live database, reads
  its schema through the backend's introspection, and generates entity
  structs plus registration functions (TOML overrides for types/class
  names/skipped tables)
- **Pluggable backends** — the core API sits on a driver-neutral backend
  interface; the connection-string scheme selects the backend
  (`odbc://...`, or a bare ODBC connection string for backward
  compatibility), and ODBC is the only backend built so far. Catalog reads are
  part of that interface (`schema()`), so a backend without a catalog answers
  `capability_not_supported` rather than failing inside a read, and every name
  there is asked for as a name rather than a pattern, so a read returns the rows
  the server reports under it. The capability list holds only the two forks the
  core takes — `columnar_batch` selects the bulk write path,
  `array_rowcount_totals` the batching of a row-counting sweep

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
changes nothing. Its `VERSION` is 0.3.0 and its `SOVERSION` is major.minor
(0.3). Identifier resolution changes public layouts and the metadata vtable:
rebuild consumers and third-party backends; do not mix 0.2 headers or binaries
with this release.

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

### Identifier spelling

A mapping can retain its declared names or fold them at SQL emission. For
unquoted DDL, PostgreSQL normally stores lowercase names; MySQL/MariaDB table
spelling also depends on server configuration. Stored spelling and the server's
case-sensitivity are different properties.

```cpp
db.identifier_case(uniorm::dialect::identifier_case::upper);  // default: keep
```

`keep` — the default — emits declared names verbatim. `lower` and `upper` fold
ASCII letters and nothing else, so a name in another alphabet cannot fold onto a
different one. Set it once, after connecting; the `orm` re-applies it to every
connection it leases, the way it does `auto_commit`. `validate()` asks the
catalog under the same policy, and a miss says which spelling the catalog has:

    table not found: USER_ACCOUNTS (only case differs from 'user_accounts')

A name the listing carries only under the other case is reported as a missing
table even where the server answers a name regardless of its case: the columns
come back with it, but they were borrowed, and the name at fault is the table.

A single fold cannot reconcile every mapping. For example, MariaDB may store
tables lowercase but preserve uppercase column spellings; its SQL can work even
when this exact-spelling validation rejects the mapping. Resolve each name
explicitly after registration instead:

```cpp
db.resolve_identifiers("app_db", "public");  // PostgreSQL
// db.resolve_identifiers("app_db", "");    // MySQL / MariaDB
db.validate();
```

Resolution selects an exact name first, then a unique ASCII-case variant.
Missing or ambiguous names throw `mapping_error`. All mappings are installed
together, or none change. Declarations remain intact, including explicit
update/remove WHERE field names; repeated resolution starts from them.

Resolved mappings use the actual quoted names, ignoring `identifier_case`.
PostgreSQL requires its current database and an explicit schema; MySQL/MariaDB
require a database and an empty schema. SQL qualifies tables with that schema
or database. Only ordinary tables are supported; there is no search-path or
cross-schema guessing, and unsupported backends fail explicitly.

`validate()` never resolves or mutates mappings. For resolved mappings it rereads
the selected identity, so a missing object does not silently select another.
`clear_identifier_resolution()` restores normal folding; connect/disconnect
clear results too, including failed reconnect attempts. Clear results before
extending a resolved mapping. New entity registrations remain unresolved until
the next resolution. Use these operations exclusively, not concurrently with
queries. Retained queries require their gateway and ORM to remain alive.
`entity_meta::populate()` expects actual column labels when resolved.

Hand-written SQL is not rewritten. Dynamic table builders are not resolved and
continue using the connection's spelling policy.

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
`--out`. `--catalog` and `--schema` narrow where tables are looked for, by name:
they are not ODBC patterns, so a `%` there matches nothing and the run says so
(empty leaves that part to the connection). `--name` renames the output unit,
`--tables` takes a comma-separated list.

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
regardless of case, and a section naming no table or column fails the run
instead of being ignored; two tables that end up sharing one class name fail it
the same way: a header declaring one struct twice is no deliverable. A `class`
override splits such a pair whenever their names differ by more than case;
names that differ by case alone share one config key, so the run asks for one of
them with `--tables` instead. See design doc §6 for the full specification.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

- **unit_tests**: pure in-memory tests, no external dependencies; they run
  against a fake backend — and, for `validate()`, a fake catalog — and link no
  ODBC, so any driver-type leak into the public API fails to compile. With
  `UNIORM_BUILD_TOOLS=ON` they also carry the generator's config and output
  tests, which name no driver any more and so live in the target that links none
- **odbc_unit_tests**: the private ODBC handle layer, its error translation and
  the catalog's name re-filter (driver manager only, no DSN required); built only
  with `UNIORM_BACKEND_ODBC=ON`
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

### Shipped

v1 is complete: the three access paths (raw SQL, aggregate projection, entity
mapping), batch writes, transactions, the pool, the dialect layer,
`uniorm::decimal_t`, and the `uniorm-gen` end-to-end flow — each verified
against a live database. The same suite passes on MariaDB, on MySQL through
Connector/ODBC with server-side prepares, and on PostgreSQL through psqlODBC.
One golden header serves all three families, because the fixture tables are
spelled the way all of them parse.

v2 is underway, and three pieces have landed on top of v1:

- **The backend abstraction** — a driver-neutral interface plus a scheme
  registry, with ODBC moved behind it and linked `PRIVATE`. A build with
  `UNIORM_BACKEND_ODBC=OFF` compiles the core library and runs its unit tests
  with no ODBC anywhere in the link line.
- **Introspection as a public contract** — `orm::schema()` hands out a
  `uniorm::schema_meta`, and the catalog reads sit behind it, in the backend
  that owns them. The generator builds through the same `orm`/`schema()` pair a
  consumer would, so it names no driver and links nothing below what
  `find_package` hands over; its config and output tests moved along, into the
  target that links none.
- **Identifier spelling as a deployment policy** — `dialect::identifier_case`,
  held by the connection and applied at the one point identifiers reach SQL, and
  `validate()` asks the catalog under the same fold. `keep` is the default and
  changes nothing for anyone who never sets it; the two folds let one generated
  header serve servers that store the same table under different case. A miss
  names the spelling the catalog has, so the policy can be read off the error.

### What CI guards

Four jobs. `core` configures the ODBC-free build over a pair of `sql.h` and
`sqlext.h` stubs that do nothing but report an error, so a driver type reaching
the public headers fails the one compile standing watch over that. `driver`
carries three legs, each pairing a connector with the server it is used
against, and each asking which server answered before it builds — PostgreSQL
answers `SHOW SERVER_VERSION`, since `SELECT VERSION()` there starts with the
server's name rather than a number. A leg whose tests skip fails, so five
passes mean the server was really reached. All four are green on the runner:
`core` over one test, each leg over five, off servers answering 8.4.11,
`11.8.9-MariaDB-ubu2404` and 17.11.

### Two answers the code had to learn to ask about

**The generator reads metadata the server answers, not SQL the dialect
writes.** Not because the SQL differs — `dialect::detect` gives both MySQL-wire
banners the same quoting and paging, and PostgreSQL the ANSI defaults — but
because the catalog the generator walks is a server answer. MariaDB connector
3.1.12 asks `information_schema` for `COLUMN_KEY = 'pri'`, matches nothing once
MySQL 8.4 declares that column `utf8mb3_bin`, and emits a header whose primary
key is simply gone — one that still compiles, registers and validates. The
comparison catches that class of silence now, on every leg: generated code
against the golden's, with everything after `//` cut from both sides, since
those comments differ per connector-and-server pair and comparing them would
nail the golden to one cell. The lost key is `.column` where the golden has
`.primary_key`; markers are left for the foreign key and the secondary index,
the two facts no line of generated code carries.

**A return value can be the driver's opinion rather than the server's fact** —
psqlODBC applies every parameter set of an array-bound UPDATE or DELETE but
leaves `SQLRowCount` at one set's count, where both MySQL-wire connectors
report the array's total. `update()` and `remove()` hand back exactly that
number, so the core asks the backend for `array_rowcount_totals` and sweeps one
set per execute when it is not there.

### Open

The cross pairings — MariaDB's connector against MySQL's server, Connector/ODBC
against MariaDB's — are out of CI by choice: what the workflow guards is the
pairing a driver is actually used with. Design doc §9 books that as a named gap,
and the gap is worth its keeping, since the one real metadata loss found so far
came out of exactly such a cell.

Next: native libpq and Oracle OCI backends — see design doc §5 and §9.
