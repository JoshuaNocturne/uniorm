#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <uniorm/schema.hpp>

#include "uniorm/error.hpp"

namespace uniorm::gen {

// A DDL script that cannot be read as a schema snapshot. Working out what was
// meant is left to the reader: a guess writes a header against a database that
// is not the one described.
class ddl_error : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

// One table as a script states it, in the shape the catalog reads answer with.
struct ddl_table {
  uniorm::schema_meta::table_ref ref;
  std::vector<uniorm::schema_meta::column_row> columns;
  std::vector<std::string> primary_key;
  std::vector<uniorm::schema_meta::foreign_key_row> foreign_keys;
  std::vector<uniorm::schema_meta::index_row> indexes;
};

// What a script describes: the database a USE names, and the tables in the
// order the script creates them.
struct ddl_snapshot {
  std::string database;
  std::vector<ddl_table> tables;
};

// Reads CREATE TABLE, CREATE INDEX and ALTER TABLE ... ADD out of a MySQL,
// MariaDB or PostgreSQL script; data, session and administration statements
// are dropped. *warnings, when set, names what a mapping cannot carry: an
// ignored schema statement, a generated column, a foreign key whose target the
// script does not create. A type outside the mapped set keeps the script's own
// spelling and answers sql_type::other, which is what a live read answers with.
ddl_snapshot parse_ddl(std::string_view script,
  std::vector<std::string>* warnings = nullptr);

// A parsed script behind the interface a live catalog answers through, so the
// generator's selection, config checks and output never know where the schema
// came from. Names come back as the script spells them.
class ddl_meta : public uniorm::schema_meta {
public:
  explicit ddl_meta(std::string_view script,
    std::vector<std::string>* warnings = nullptr);

  std::string database_name() override;
  std::vector<table_row> tables(
    std::string_view catalog, std::string_view schema) override;
  std::vector<column_row> table_columns(table_ref const& ref) override;
  std::vector<std::string> primary_key(table_ref const& ref) override;
  std::vector<foreign_key_row> foreign_keys(table_ref const& ref) override;
  std::vector<index_row> indexes(table_ref const& ref) override;

private:
  ddl_table const* find(table_ref const& ref) const;

  std::vector<ddl_table> tables_;
  std::string database_;
};

}  // namespace uniorm::gen
