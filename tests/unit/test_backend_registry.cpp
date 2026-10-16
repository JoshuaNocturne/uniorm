#include "check.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/connection_spec.hpp>

namespace {

using uniorm::backend::parse_scheme;
using uniorm::backend::registry;

void test_parse_scheme() {
  // Bare ODBC strings keep the whole string as the tail.
  auto bare = parse_scheme("DSN=mydb;UID=u;PWD=p");
  CHECK(bare.scheme == "odbc");
  CHECK(bare.tail == "DSN=mydb;UID=u;PWD=p");

  // Explicit scheme strips the prefix.
  auto odbc = parse_scheme("odbc://DSN=mydb;UID=u;PWD=p");
  CHECK(odbc.scheme == "odbc");
  CHECK(odbc.tail == "DSN=mydb;UID=u;PWD=p");

  auto pg = parse_scheme("postgres://host=h dbname=d");
  CHECK(pg.scheme == "postgres");
  CHECK(pg.tail == "host=h dbname=d");

  // Schemes are lowercased.
  CHECK(parse_scheme("ODBC://DSN=x").scheme == "odbc");

  // Quirks that look like schemes but are not stay bare ODBC.
  auto tcp = parse_scheme("SERVER=tcp://host");
  CHECK(tcp.scheme == "odbc");
  CHECK(tcp.tail == "SERVER=tcp://host");
  CHECK(parse_scheme("9bad://x").scheme == "odbc");
  CHECK(parse_scheme("://x").scheme == "odbc");

  // Empty string and empty tail.
  CHECK(parse_scheme("").scheme == "odbc");
  CHECK(parse_scheme("").tail == "");
  CHECK(parse_scheme("odbc://").tail == "");
}

// Minimal factory product for registration tests: never opened.
struct dummy_connection : uniorm::backend::connection_iface {
  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return false;
  }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  uniorm::backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "dummy";
  }
  std::unique_ptr<uniorm::backend::statement_iface>
  create_statement() override {
    return nullptr;
  }
  void* native_handle() noexcept override {
    return nullptr;
  }
};

// A backend that does take the four pieces, so the handover itself is
// observable: the core reads the scheme to pick a factory and passes the rest
// through untouched, whatever the pieces look like.
struct spec_recording_connection : dummy_connection {
  uniorm::connection_spec seen;
  int opens = 0;
  void open(uniorm::connection_spec const& spec) override {
    seen = spec;
    ++opens;
  }
};

spec_recording_connection* recorded = nullptr;

void test_registry() {
  registry& reg = registry::instance();

  // This target deliberately links no backend, so the registry starts empty
  // here whatever the build had compiled; the split is exactly what makes
  // that visible. The install smoke is where a linked backend's
  // self-registration gets asserted.
  CHECK(!reg.contains("odbc"));
  CHECK(!reg.contains("dummy"));
  reg.register_backend(
    "dummy", [] { return std::make_unique<dummy_connection>(); });
  CHECK(reg.contains("dummy"));

  std::string tail;
  auto conn = reg.create("dummy://abc=1", &tail);
  CHECK(conn != nullptr);
  CHECK(tail == "abc=1");

  // Duplicate registration is rejected.
  CHECK_THROWS(reg.register_backend(
                 "dummy", [] { return std::make_unique<dummy_connection>(); }),
    uniorm::backend::backend_error);

  // Unknown schemes throw and list what is registered.
  CHECK_THROWS(
    reg.create("nosuch://x", nullptr), uniorm::backend::unknown_scheme);

  auto schemes = reg.schemes();
  CHECK(std::find(schemes.begin(), schemes.end(), "dummy") != schemes.end());

  // dummy_connection answers no introspection, so the connection layer is
  // the one that says so.
  uniorm::connection bare("dummy://abc=1");
  CHECK_THROWS(bare.schema(), uniorm::backend::capability_not_supported);
}

void test_create_for_scheme() {
  registry& reg = registry::instance();
  reg.register_backend(
    "specfake", [] { return std::make_unique<dummy_connection>(); });

  CHECK(reg.create_for_scheme("specfake") != nullptr);
  // The string route lowercases the prefix it found before looking it up,
  // and a spec has to land on the same spelling.
  CHECK(reg.create_for_scheme("SPECFAKE") != nullptr);

  CHECK_THROWS(
    reg.create_for_scheme("nosuch"), uniorm::backend::unknown_scheme);
  CHECK_THROWS(
    reg.create_for_scheme(""), uniorm::backend::unknown_scheme);
}

void test_spec_open_is_optional() {
  // dummy_connection overrides only the string form: leaving the four-piece
  // form alone is a statement about the backend, and the core reports it.
  uniorm::connection_spec spec{ "dummy", "where", "who", "secret" };
  CHECK_THROWS(uniorm::connection(spec),
    uniorm::backend::capability_not_supported);
}

void test_pieces_reach_the_backend_as_written() {
  registry& reg = registry::instance();
  reg.register_backend(
    "specraw", [] {
      auto conn = std::make_unique<spec_recording_connection>();
      recorded = conn.get();
      return conn;
    });

  // The scheme is folded to lowercase for the lookup only, so a spec naming it
  // another way still lands on this factory; what the backend is handed stays
  // as written, since spelling a target out is that backend's own business.
  uniorm::connection_spec spec{ "SPECRAW", "a;b", "", "p=ss" };
  {
    uniorm::connection conn(spec);
    CHECK(recorded->opens == 1);
    CHECK(recorded->seen.scheme == "SPECRAW");
    CHECK(recorded->seen.target == "a;b");
    CHECK(recorded->seen.user.empty());
    CHECK(recorded->seen.password == "p=ss");
  }
}

}  // namespace

void test_backend_registry() {
  test_parse_scheme();
  test_registry();
  test_create_for_scheme();
  test_spec_open_is_optional();
  test_pieces_reach_the_backend_as_written();
}
