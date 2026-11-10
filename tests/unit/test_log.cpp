#include "check.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/backend/registry.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/log.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/pool.hpp>
#include <uniorm/transaction.hpp>

using namespace uniorm;

namespace {

struct captured_record {
  log_level level;
  std::string site;
  std::string message;
};

std::vector<captured_record>& taken() {
  static std::vector<captured_record> records;
  return records;
}

void capture(log_record const& record) {
  taken().push_back(
    { record.level, std::string(record.site), std::string(record.message) });
}

// The state the library starts in, put back so a later test in this process
// finds no sink of somebody else's.
void reset_logging() {
  set_log_sink({});
  set_log_level(log_level::info);
}

void install_capturing_sink() {
  taken().clear();
  set_log_level(log_level::trace);
  set_log_sink(capture);
}

captured_record const* only_record() {
  return taken().size() == 1 ? &taken().front() : nullptr;
}

// Rolls every statement back and says so, which is the failure a dropped
// rollback record is written about.
class stubborn_connection : public backend::connection_iface {
public:
  void open(std::string_view) override {}
  void close() override {}
  bool is_open() const noexcept override {
    return true;
  }
  void set_autocommit(bool enabled) override {
    autocommit_ = enabled;
  }
  void commit() override {}
  void rollback() override {
    throw std::runtime_error("the server refused to roll back");
  }
  backend::capabilities caps() const noexcept override {
    return {};
  }
  std::string dbms_name() const override {
    return "stubborn";
  }
  std::unique_ptr<backend::statement_iface> create_statement() override {
    return nullptr;
  }
  void* native_handle() noexcept override {
    return nullptr;
  }

  bool autocommit_ = true;
};

void register_stubborn_backend() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  backend::registry::instance().register_backend(
    "logstubborn", [] { return std::make_unique<stubborn_connection>(); });
}

void test_log_hands_a_record_to_the_sink() {
  install_capturing_sink();
  detail::log(log_level::warn, "core.test", "one sentence");
  captured_record const* record = only_record();
  CHECK(record != nullptr);
  if (record != nullptr) {
    CHECK(record->level == log_level::warn);
    CHECK(record->site == "core.test");
    CHECK(record->message == "one sentence");
  }
  reset_logging();
}

void test_log_threshold_gates_what_is_built() {
  install_capturing_sink();
  set_log_level(log_level::error);
  CHECK(log_level_threshold() == log_level::error);
  CHECK(log_enabled(log_level::error));
  CHECK(!log_enabled(log_level::warn));

  int builds = 0;
  detail::log(log_level::debug, "core.test", [&] {
    ++builds;
    return std::string("should not have been assembled");
  });
  CHECK(builds == 0);
  CHECK(taken().empty());

  detail::log(log_level::error, "core.test", [&] {
    ++builds;
    return std::string("assembled once");
  });
  CHECK(builds == 1);
  CHECK(only_record() != nullptr);
  reset_logging();
}

void test_log_is_silent_without_a_sink() {
  reset_logging();
  taken().clear();
  CHECK(!log_enabled(log_level::error));
  detail::log(log_level::error, "core.test", "nobody is listening");
  detail::log(log_level::warn, "core.test",
    [] { return std::string("nobody is listening either"); });
  CHECK(taken().empty());
}

void test_log_drops_what_a_throwing_sink_throws() {
  install_capturing_sink();
  set_log_sink(
    [](log_record const&) { throw std::runtime_error("the sink broke"); });
  bool survived = false;
  try {
    detail::log(log_level::error, "core.test", "a failure worth reporting");
    survived = true;
  } catch (...) {
  }
  CHECK(survived);
  reset_logging();
}

void test_log_reports_a_reset_the_pool_gave_up_on() {
  register_stubborn_backend();
  install_capturing_sink();
  uniorm::pool_options opts;
  opts.target = "logstubborn://place";
  opts.size = 1;
  opts.acquire_timeout = std::chrono::milliseconds(200);
  opts.heartbeat_interval = std::chrono::milliseconds(0);  // no maintainer
  {
    uniorm::connection_pool pool(opts);
    {
      uniorm::orm db(pool);
      db.auto_commit(false);
    }
    // The lease is gone with manual mode still on it, and the reset that would
    // have cleared that is the failure the record is written about.
  }
  captured_record const* record = only_record();
  CHECK(record != nullptr);
  if (record != nullptr) {
    CHECK(record->level == log_level::warn);
    CHECK(record->site == "core.pool");
    CHECK(record->message.find("could not be reset") != std::string::npos);
    CHECK(record->message.find("the server refused to roll back") !=
          std::string::npos);
  }
  reset_logging();
}

void test_log_reports_a_dropped_rollback() {
  register_stubborn_backend();
  install_capturing_sink();
  {
    connection conn("logstubborn://place");
    transaction txn(conn);
    // Leaving the transaction live is the point: its destructor is what has to
    // stay silent, and the record is what says why it may not be.
  }
  captured_record const* record = only_record();
  CHECK(record != nullptr);
  if (record != nullptr) {
    CHECK(record->level == log_level::warn);
    CHECK(record->site == "core.transaction");
    CHECK(record->message.find("the server refused to roll back") !=
          std::string::npos);
  }
  reset_logging();
}

}  // namespace

void test_log() {
  test_log_hands_a_record_to_the_sink();
  test_log_threshold_gates_what_is_built();
  test_log_is_silent_without_a_sink();
  test_log_drops_what_a_throwing_sink_throws();
  test_log_reports_a_dropped_rollback();
  test_log_reports_a_reset_the_pool_gave_up_on();
}
