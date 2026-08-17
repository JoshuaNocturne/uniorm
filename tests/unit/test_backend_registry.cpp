#include "check.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/error.hpp>
#include <uniorm/backend/registry.hpp>

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
  bool is_open() const noexcept override { return false; }
  void set_autocommit(bool) override {}
  void commit() override {}
  void rollback() override {}
  uniorm::backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override { return "dummy"; }
  std::unique_ptr<uniorm::backend::statement_iface> create_statement()
    override {
    return nullptr;
  }
  void* native_handle() noexcept override { return nullptr; }
};

void test_registry() {
  registry& reg = registry::instance();

  // The ODBC backend compiled into libuniorm self-registers at load.
  CHECK(reg.contains("odbc"));

  CHECK(!reg.contains("fake"));
  reg.register_backend(
    "fake", [] { return std::make_unique<dummy_connection>(); });
  CHECK(reg.contains("fake"));

  std::string tail;
  auto conn = reg.create("fake://abc=1", &tail);
  CHECK(conn != nullptr);
  CHECK(tail == "abc=1");

  // Duplicate registration is rejected.
  CHECK_THROWS(reg.register_backend("fake", [] {
                 return std::make_unique<dummy_connection>();
               }),
    uniorm::backend::backend_error);

  // Unknown schemes throw and list what is registered.
  CHECK_THROWS(reg.create("nosuch://x", nullptr),
    uniorm::backend::unknown_scheme);

  auto schemes = reg.schemes();
  CHECK(std::find(schemes.begin(), schemes.end(), "fake") !=
        schemes.end());
}

}  // namespace

void test_backend_registry() {
  test_parse_scheme();
  test_registry();
}
