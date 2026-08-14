#include "generator.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

#include "naming.hpp"
#include "uniorm/types.hpp"

namespace uniorm::gen {

namespace {

std::string to_upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return std::toupper(c); });
  return s;
}

std::string normalize_type(std::string t) {
  t.erase(std::remove_if(t.begin(), t.end(),
            [](unsigned char c) { return std::isspace(c); }),
    t.end());
  return t;
}

bool is_bindable_cpp_type(std::string const& t) {
  static std::unordered_set<std::string> const allowed = {
    "bool",
    "std::int8_t",
    "std::int16_t",
    "std::int32_t",
    "std::int64_t",
    "double",
    "std::string",
    "std::vector<std::byte>",
    "uniorm::timestamp",
    "timestamp",
  };
  return allowed.count(normalize_type(t)) != 0;
}

std::string check_bindable(std::string const& t, std::string const& where) {
  if (!is_bindable_cpp_type(t)) {
    throw config_error(where + ": cpp type '" + t +
                       "' cannot be bound by the v1 registry (allowed: "
                       "bool, std::int8_t..int64_t, double, std::string, "
                       "std::vector<std::byte>, uniorm::timestamp)");
  }
  return normalize_type(t);
}

std::string default_cpp_type(
  column_model const& col, std::string const& where, generated_output& out) {
  switch (sql_type_from_native(col.data_type)) {
  case sql_type::boolean:
    return "bool";
  case sql_type::smallint:
    return "std::int16_t";
  case sql_type::integer:
    return "std::int32_t";
  case sql_type::bigint:
    return "std::int64_t";
  case sql_type::real:
    out.warnings.push_back(
      where + ": REAL/FLOAT mapped to double (float is not bindable)");
    return "double";
  case sql_type::double_precision:
    return "double";
  case sql_type::decimal:
#ifdef UNIORM_DECIMAL_AS_DOUBLE
    return "double";
#else
    return "std::string";
#endif
  case sql_type::character:
  case sql_type::varchar:
  case sql_type::longvarchar:
  case sql_type::wchar:
  case sql_type::wvarchar:
  case sql_type::guid:
    return "std::string";
  case sql_type::binary:
  case sql_type::varbinary:
    return "std::vector<std::byte>";
  case sql_type::date:
  case sql_type::time:
  case sql_type::timestamp:
    return "uniorm::timestamp";
  case sql_type::other:
  default:
    out.warnings.push_back(
      where + ": unmapped SQL type '" + col.type_name + "', using std::string");
    return "std::string";
  }
}

std::string const* find_type_override(
  column_model const& col, gen_config const& cfg) {
  std::string base = to_upper(col.type_name);
  std::string with_ps = base + "(" + std::to_string(col.size) + "," +
                        std::to_string(col.decimals) + ")";
  auto it = cfg.type_overrides.find(with_ps);
  if (it != cfg.type_overrides.end()) {
    return &it->second;
  }
  it = cfg.type_overrides.find(base);
  if (it != cfg.type_overrides.end()) {
    return &it->second;
  }
  return nullptr;
}

std::string column_comment(column_model const& col) {
  std::string comment = col.type_name;
  if (col.size > 0) {
    comment += "(" + std::to_string(col.size);
    if (col.decimals > 0) {
      comment += "," + std::to_string(col.decimals);
    }
    comment += ")";
  }
  if (col.primary_key) {
    comment += " PK";
  }
  if (!col.nullable) {
    comment += " NOT NULL";
  }
  if (col.default_value) {
    comment += " DEFAULT " + *col.default_value;
  }
  return comment;
}

struct member_info {
  std::string member;
  std::string cpp_type;
  column_model const* col;
};

std::vector<member_info> build_members(
  table_model const& table, gen_config const& cfg, generated_output& out) {
  auto table_it = cfg.tables.find(table.name);
  table_config const* tcfg =
    table_it != cfg.tables.end() ? &table_it->second : nullptr;

  std::vector<member_info> members;
  std::unordered_set<std::string> used;
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    column_model const& col = table.columns[i];
    std::string where = table.name + "." + col.name;

    column_override const* ovr = nullptr;
    if (tcfg != nullptr) {
      auto c_it = tcfg->columns.find(col.name);
      if (c_it != tcfg->columns.end()) {
        ovr = &c_it->second;
      }
    }
    if (ovr != nullptr && ovr->converter) {
      throw config_error(
        where +
        ": converter-backed members are not supported by the v1 registry");
    }

    std::string cpp_type;
    if (ovr != nullptr && ovr->cpp_type) {
      cpp_type = check_bindable(*ovr->cpp_type, where);
    } else if (std::string const* global = find_type_override(col, cfg)) {
      cpp_type = check_bindable(*global, where);
    } else {
      cpp_type = default_cpp_type(col, where, out);
    }

    std::string member = to_camel_case(col.name);
    if (used.count(member) != 0) {
      member += std::to_string(i);
      out.warnings.push_back(
        where + ": member name collision, renamed to '" + member + "'");
    }
    used.insert(member);
    members.push_back(member_info{ member, cpp_type, &col });
  }
  return members;
}

void emit_table(std::string& text, table_model const& table,
  gen_config const& cfg, generated_output& out) {
  auto table_it = cfg.tables.find(table.name);
  if (table_it != cfg.tables.end() && table_it->second.skip) {
    return;
  }
  std::string class_name =
    (table_it != cfg.tables.end() && table_it->second.class_name)
      ? *table_it->second.class_name
      : to_pascal_case(table.name);

  std::vector<member_info> members = build_members(table, cfg, out);

  text += "// table: " + table.name + "\n";
  for (fk_model const& fk : table.foreign_keys) {
    for (auto const& [from, to] : fk.columns) {
      text += "// FK: " + from + " -> " + fk.pk_table + "(" + to + ")\n";
    }
  }
  for (index_model const& idx : table.indexes) {
    text += "// index: " + idx.name + " (";
    for (std::size_t i = 0; i < idx.columns.size(); ++i) {
      if (i != 0) {
        text += ", ";
      }
      text += idx.columns[i];
    }
    text += ")";
    if (idx.unique) {
      text += " UNIQUE";
    }
    text += "\n";
  }

  text += "struct " + class_name + " {\n";
  for (member_info const& m : members) {
    std::string decl = m.cpp_type;
    if (m.col->nullable) {
      decl = "std::optional<" + decl + ">";
    }
    text +=
      "  " + decl + " " + m.member + ";  // " + column_comment(*m.col) + "\n";
  }
  text += "};\n\n";

  text += "inline void register_" + class_name +
          "_mapping(uniorm::orm& registry) {\n";
  text += "  registry.map<" + class_name + ">(\"" + table.name + "\")\n";
  for (std::size_t i = 0; i < members.size(); ++i) {
    member_info const& m = members[i];
    std::string call =
      m.col->primary_key ? "    .primary_key(" : "    .column(";
    call += "\"" + m.col->name + "\", &" + class_name + "::" + m.member + ")";
    text += call + "\n";
  }
  // Replace the trailing newline with a semicolon.
  text.pop_back();
  text += ";\n}\n\n";
}

}  // namespace

generated_output generate_header(
  schema_model const& model, gen_config const& cfg) {
  generated_output out;
  std::string& text = out.text;
  std::string const& unit = model.name;

  text += "// Generated by uniorm-gen. Do not edit.\n";
  text += "#pragma once\n\n";
  text += "#include <cstddef>\n";
  text += "#include <cstdint>\n";
  text += "#include <optional>\n";
  text += "#include <string>\n";
  text += "#include <vector>\n\n";
  text += "#include <uniorm/mapping/registry.hpp>\n\n";
  text += "namespace " + unit + " {\n\n";

  for (table_model const& table : model.tables) {
    emit_table(text, table, cfg, out);
  }

  text += "inline void register_" + unit + "_schema(uniorm::orm& registry) {\n";
  for (table_model const& table : model.tables) {
    auto table_it = cfg.tables.find(table.name);
    if (table_it != cfg.tables.end() && table_it->second.skip) {
      continue;
    }
    std::string class_name =
      (table_it != cfg.tables.end() && table_it->second.class_name)
        ? *table_it->second.class_name
        : to_pascal_case(table.name);
    text += "  register_" + class_name + "_mapping(registry);\n";
  }
  text += "}\n\n";
  text += "}  // namespace " + unit + "\n";
  return out;
}

}  // namespace uniorm::gen
