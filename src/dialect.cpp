#include "uniorm/dialect.hpp"

#include "identifier.hpp"
#include <uniorm/error.hpp>

namespace uniorm {

std::string dialect::fold_identifier(std::string_view identifier) const {
  switch (identifiers) {
  case identifier_case::lower:
    return detail::fold_lower(identifier);
  case identifier_case::upper:
    return detail::fold_upper(identifier);
  case identifier_case::keep:
    break;
  }
  return std::string(identifier);
}

std::string dialect::quote_identifier(std::string_view identifier) const {
  return quote_exact_identifier(fold_identifier(identifier));
}

std::string dialect::quote_exact_identifier(std::string_view identifier) const {
  if (identifier.find('\0') != std::string_view::npos) {
    throw uniorm_error("identifier contains a NUL byte");
  }
  std::string quoted;
  quoted.reserve(identifier.size() + 2);
  quoted.push_back(quote_open);
  for (char character : identifier) {
    quoted.push_back(character);
    if (character == quote_close) {
      quoted.push_back(quote_close);
    }
  }
  quoted.push_back(quote_close);
  return quoted;
}

std::string dialect::pagination(
  std::optional<std::size_t> limit, std::size_t offset) const {
  if (!limit && offset == 0) {
    return {};
  }
  std::string out;
  if (ansi_pagination) {
    if (offset > 0) {
      out += " OFFSET " + std::to_string(offset) + " ROWS";
    }
    if (limit) {
      out += std::string(
               out.empty() ? " OFFSET 0 ROWS FETCH NEXT " : " FETCH NEXT ") +
             std::to_string(*limit) + " ROWS ONLY";
    }
  } else {
    if (limit) {
      out += " LIMIT " + std::to_string(*limit);
    }
    if (offset > 0) {
      out += " OFFSET " + std::to_string(offset);
    }
  }
  return out;
}

namespace {

// The first release whose FROM clause takes several unnest() arguments at
// once, which is the call that lays a batch's columns beside each other. An
// older server answers it with an unknown function, so the array row source is
// only offered at or above this version.
constexpr std::size_t array_row_source_floor = 904;

// The releases that grew a VALUES table constructor in the FROM clause, which
// replaces the seeded UNION ALL chain with one list. SQL Server has had it
// since 2008 (10.0); Oracle only since 23ai (23). Below these the family keeps
// the tuples it can build from nothing beside the statement.
constexpr std::size_t sql_server_values_floor = 1000;
constexpr std::size_t oracle_values_floor = 2300;

// The digits starting at `position`, with `position` left past them. A
// position that is not on a digit reads zero and does not move.
std::size_t read_number(std::string_view version, std::size_t& position) {
  std::size_t value = 0;
  while (position < version.size()) {
    char const symbol = version[position];
    if (symbol < '0' || symbol > '9') {
      break;
    }
    value = value * 10 + static_cast<std::size_t>(symbol - '0');
    ++position;
  }
  return value;
}

// The first two numbers of a version as one comparable value: "9.4.20" gives
// 904, "18.6" gives 1806, a bare "10" gives 1000. Words that carry no digits
// at all answer 0, which reads as a server that did not say.
std::size_t parse_server_version(std::string_view version) {
  std::size_t position = version.find_first_of("0123456789");
  if (position == std::string_view::npos) {
    return 0;
  }
  std::size_t const major = read_number(version, position);
  std::size_t minor = 0;
  if (position < version.size() && version[position] == '.') {
    ++position;
    minor = read_number(version, position);
  }
  return major * 100 + minor;
}

}  // namespace

dialect dialect::detect(std::string_view dbms_name) {
  std::string lower = detail::fold_lower(dbms_name);
  dialect detected;
  if (lower.find("mysql") != std::string::npos ||
      lower.find("mariadb") != std::string::npos) {
    detected.quote_open = '`';
    detected.quote_close = '`';
    detected.ansi_pagination = false;
    detected.table_qualification = qualification::catalog;
    detected.batch_write = batch_family::mysql;
    // No ceiling here was read off a server: this family's folded shape is
    // the one the core has not run against a live MySQL or MariaDB yet.
    detected.fold_parameter_ceiling = 4000;
  } else if (lower.find("postgresql") != std::string::npos) {
    // The same habit one server over, in the other direction: an unquoted
    // name is stored lower-cased, so a quoted read of the declared spelling
    // would miss the table the same statement created.
    detected.table_qualification = qualification::schema;
    detected.identifiers = identifier_case::lower;
    detected.batch_write = batch_family::postgresql;
    // psqlODBC answers a 9000-parameter fold by losing the connection, and a
    // 6000-parameter one exactly; the ceiling leaves a chunk of margin.
    detected.fold_parameter_ceiling = 4000;
  } else if (lower.find("oracle") != std::string::npos) {
    // What a table belongs to is called a schema here and nowhere else. An
    // unquoted name is also stored upper-cased, so the fold comes with the
    // banner rather than waiting on the deployment to declare it.
    detected.table_qualification = qualification::schema;
    detected.identifiers = identifier_case::upper;
    detected.batch_write = batch_family::oracle;
    // Oracle's ODBC driver handed back a truncated statement past roughly
    // 64 KB of text, which a 3000-parameter fold stays under.
    detected.fold_parameter_ceiling = 3000;
  } else if (lower.find("sql server") != std::string::npos) {
    detected.batch_write = batch_family::sql_server;
    // The server's own limit is 2100 parameters per statement.
    detected.fold_parameter_ceiling = 2000;
  }
  return detected;
}

// What the server's own words for itself say about the row sources this family
// knows. An answer that carries no version leaves the shape that family has
// always had, which is the one the core can build from nothing beside the
// statement itself.
void dialect::accept_server_version(std::string_view version) {
  server_version = parse_server_version(version);
  switch (batch_write) {
  case batch_family::postgresql:
    batch_rows = server_version >= array_row_source_floor
                   ? batch_source::typed_arrays
                   : batch_source::parameter_tuples;
    break;
  case batch_family::sql_server:
    batch_rows = server_version >= sql_server_values_floor
                   ? batch_source::value_constructors
                   : batch_source::parameter_tuples;
    break;
  case batch_family::oracle:
    batch_rows = server_version >= oracle_values_floor
                   ? batch_source::value_constructors
                   : batch_source::parameter_tuples;
    break;
  case batch_family::mysql:
  case batch_family::none:
    break;
  }
}

}  // namespace uniorm
