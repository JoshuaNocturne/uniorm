// validate's checks behind the connection: a fake catalog answers with the
// spellings a server would, and a miss has to name them.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "check.hpp"

#include "orm_mapping.hpp"

#include <uniorm/error.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/schema.hpp>
#include <uniorm/types.hpp>

using namespace uniorm;

namespace {

struct Account {
  std::int64_t userId = 0;
  std::string name;
  std::optional<std::int32_t> balance;
};

// The one table this catalog has, spelled and typed as the server spells it.
class fake_catalog : public schema_meta {
public:
  std::string table;
  std::string listed_schema;
  table_shape shape;
  std::vector<std::string> names;

  std::string database_name() override {
    return "fake";
  }

  std::vector<table_row> tables(
    std::string_view, std::string_view) override {
    std::vector<table_row> out;
    for (auto const& name : names) {
      out.push_back(table_row{ {}, listed_schema, name });
    }
    return out;
  }

  std::vector<column_row> table_columns(table_ref const& ref) override {
    if (ref.name != table) {
      return {};
    }
    std::vector<column_row> out;
    for (auto const& column : shape) {
      out.push_back(column_row{ column, {}, 0, 0, std::nullopt });
    }
    return out;
  }

  std::vector<std::string> primary_key(table_ref const&) override {
    return {};
  }
  std::vector<foreign_key_row> foreign_keys(table_ref const&) override {
    return {};
  }
  std::vector<index_row> indexes(table_ref const&) override {
    return {};
  }
};

// The message a rejection carries, empty when the mapping passed. The
// spelling policy is the default one unless a case says otherwise.
std::string failure(schema_meta& md, entity_meta const& m,
  validation_mode mode, dialect const& d = {}) {
  try {
    detail::validate_entity(md, m, mode, d);
  } catch (uniorm_error const& e) {
    return e.what();
  }
  return {};
}

fake_catalog catalog_of(std::string table, std::vector<column_shape> columns) {
  fake_catalog md;
  md.table = std::move(table);
  md.shape = std::move(columns);
  md.names = { md.table };
  return md;
}

}  // namespace

void test_orm_validate() {
  orm registry;
  registry.map<Account>("USER_ACCOUNTS")
    .primary_key("USER_ID", &Account::userId)
    .column("NAME", &Account::name)
    .column("BALANCE", &Account::balance);
  entity_meta const& m = registry.meta<Account>();

  auto good = catalog_of("USER_ACCOUNTS", {
    { "USER_ID", sql_type::bigint, false },
    { "NAME", sql_type::varchar, false },
    { "BALANCE", sql_type::integer, true },
  });
  CHECK(failure(good, m, validation_mode::strict).empty());
  CHECK(failure(good, m, validation_mode::lenient).empty());

  // Strict compares the family; lenient is for a server whose types the
  // mapping was not written against.
  auto date_key = catalog_of("USER_ACCOUNTS", {
    { "USER_ID", sql_type::date, false },
    { "NAME", sql_type::varchar, false },
    { "BALANCE", sql_type::integer, true },
  });
  CHECK(failure(date_key, m, validation_mode::lenient).empty());
  CHECK(failure(date_key, m, validation_mode::strict) ==
    "column USER_ACCOUNTS.USER_ID is DATE, which the mapped member does not "
    "bind");

  auto nullable_name = catalog_of("USER_ACCOUNTS", {
    { "USER_ID", sql_type::bigint, false },
    { "NAME", sql_type::varchar, true },
    { "BALANCE", sql_type::integer, true },
  });
  CHECK(failure(nullable_name, m, validation_mode::strict) ==
    "column USER_ACCOUNTS.NAME is nullable but the mapped member is not "
    "std::optional");

  // A miss says what the catalog spells, in both places a name can miss.
  auto lower_table = catalog_of("user_accounts", {
    { "USER_ID", sql_type::bigint, false },
    { "NAME", sql_type::varchar, false },
    { "BALANCE", sql_type::integer, true },
  });
  CHECK(failure(lower_table, m, validation_mode::strict) ==
    "table not found: USER_ACCOUNTS (only case differs from 'user_accounts')");

  auto absent = catalog_of("t_sessions", {});
  CHECK(failure(absent, m, validation_mode::strict) ==
    "table not found: USER_ACCOUNTS");

  // The search is over the whole catalog, so a candidate says where it lives:
  // another schema's table is not the one this mapping wanted.
  auto elsewhere = catalog_of("user_accounts", {
    { "USER_ID", sql_type::bigint, false },
    { "NAME", sql_type::varchar, false },
    { "BALANCE", sql_type::integer, true },
  });
  elsewhere.listed_schema = "hr";
  CHECK(failure(elsewhere, m, validation_mode::strict) ==
    "table not found: USER_ACCOUNTS (only case differs from "
    "'hr.user_accounts')");

  auto lower_columns = catalog_of("USER_ACCOUNTS", {
    { "user_id", sql_type::bigint, false },
    { "name", sql_type::varchar, false },
    { "balance", sql_type::integer, true },
  });
  CHECK(failure(lower_columns, m, validation_mode::strict) ==
    "column not found in table USER_ACCOUNTS: USER_ID (only case differs "
    "from 'user_id')");

  auto other_columns = catalog_of("USER_ACCOUNTS", {
    { "id", sql_type::bigint, false },
  });
  CHECK(failure(other_columns, m, validation_mode::strict) ==
    "column not found in table USER_ACCOUNTS: USER_ID");

  // The spelling policy is part of what a mapping is checked against, so one
  // header spelled uppercase validates against a server that stores lower.
  auto all_lower = catalog_of("user_accounts", {
    { "user_id", sql_type::bigint, false },
    { "name", sql_type::varchar, false },
    { "balance", sql_type::integer, true },
  });
  dialect policy_lower;
  policy_lower.identifiers = dialect::identifier_case::lower;
  CHECK(failure(all_lower, m, validation_mode::strict, policy_lower).empty());
  // A miss reports the name the read asked for, so a policy that disagrees
  // with its server says which name it looked up.
  CHECK(failure(good, m, validation_mode::strict, policy_lower) ==
    "table not found: user_accounts (only case differs from 'USER_ACCOUNTS')");

  dialect policy_upper;
  policy_upper.identifiers = dialect::identifier_case::upper;
  CHECK(failure(good, m, validation_mode::strict, policy_upper).empty());
  CHECK(failure(all_lower, m, validation_mode::strict, policy_upper) ==
    "table not found: USER_ACCOUNTS (only case differs from 'user_accounts')");
}
