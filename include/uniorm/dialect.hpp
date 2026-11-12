#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <uniorm/export.hpp>

namespace uniorm {

// Minimal per-database SQL generation quirks: identifier quoting and
// pagination syntax.
struct UNIORM_API dialect {
  // How generated SQL spells the identifiers it quotes. A server decides
  // whether a name is case-sensitive and what it stores. A dialect built by
  // hand keeps; detect() answers for a server whose habit it knows, and a
  // deployment can override that answer.
  enum class identifier_case { keep, lower, upper };
  enum class qualification { unsupported, schema, catalog };
  // The shape a batch of keys and new values takes as one statement. `none`
  // means no folded shape is known for this server, and a batch write stays on
  // the backend's own array of parameter sets.
  enum class batch_family { none, postgresql, mysql, oracle, sql_server };
  // How a folded batch spells the rows it carries. Tuples are one parameter
  // group per row, seeded from the table so no value names a type; arrays are
  // one parameter per column, and each column's type has to be spelled out;
  // value constructors are one VALUES list of parameter groups, the column
  // names carried on the alias instead of by a seed SELECT.
  enum class batch_source {
    parameter_tuples,
    typed_arrays,
    value_constructors
  };

  char quote_open = '"';
  char quote_close = '"';
  bool ansi_pagination = true;  // false => LIMIT/OFFSET style
  identifier_case identifiers = identifier_case::keep;
  qualification table_qualification = qualification::unsupported;
  batch_family batch_write = batch_family::none;
  // The most parameters one folded statement of this family may carry. A
  // chunk of rows costs rows times columns, so the core cuts a folded batch
  // under this ceiling.
  std::size_t fold_parameter_ceiling = 0;
  // What a folded batch of this dialect's family is built from. The array
  // shape is the one a server only grew in some release, so it arrives here
  // through accept_server_version() rather than from detect() alone.
  batch_source batch_rows = batch_source::parameter_tuples;
  // The server's own version as one comparable number: 9.4 lands on 904, 10
  // on 1000, 18.6 on 1806. Zero when the server did not say.
  std::size_t server_version = 0;

  // The name as this dialect will put it in SQL, quotes aside. Catalog reads
  // ask by it, so a miss is reported in the spelling the statement uses.
  std::string fold_identifier(std::string_view identifier) const;
  std::string quote_identifier(std::string_view identifier) const;
  std::string quote_exact_identifier(std::string_view identifier) const;
  std::string pagination(
    std::optional<std::size_t> limit, std::size_t offset) const;

  static dialect detect(std::string_view dbms_name);

  // Records the version the server says it is, and with it which of the
  // family's shapes this server can answer. detect() made the family, and a
  // version read from a server that has no family to attach it to changes
  // nothing.
  void accept_server_version(std::string_view version);
};

}  // namespace uniorm
