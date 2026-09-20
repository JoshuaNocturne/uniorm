#include "generator.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "naming.hpp"
#include "uniorm/types.hpp"

namespace uniorm::gen {

namespace {

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
    "uniorm::decimal_t",
    "decimal_t",
  };
  return allowed.count(normalize_type(t)) != 0;
}

std::string check_bindable(std::string const& t, std::string const& where) {
  if (!is_bindable_cpp_type(t)) {
    throw config_error(where + ": cpp type '" + t +
                       "' cannot be bound by the v1 registry (allowed: "
                       "bool, std::int8_t..int64_t, double, std::string, "
                       "std::vector<std::byte>, uniorm::timestamp, "
                       "uniorm::decimal_t; a domain type goes under "
                       "converter = \"...\")");
  }
  return normalize_type(t);
}

std::string default_cpp_type(
  column_model const& col, std::string const& where, generated_output& out) {
  switch (col.shape.type) {
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
  case sql_type::decimal:  // exact literal; [types] or cpp_type can override
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
  std::string base = fold_upper(col.type_name);
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
  if (!col.shape.nullable) {
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
  // The member type is a domain type the registry reaches through
  // uniorm::converter<T>, so nothing here can check what it binds.
  bool through_converter = false;
};

// The override block a config section leaves for this table, matched with the
// catalog name folded the way config keys are stored.
table_config const* table_config_for(
  gen_config const& cfg, table_model const& table) {
  auto it = cfg.tables.find(fold_lower(table.name));
  return it != cfg.tables.end() ? &it->second : nullptr;
}

// The class a table contributes, nothing when the config skips it. One answer
// feeds both the struct and the register function, so they cannot drift.
std::optional<std::string> emitted_class(
  gen_config const& cfg, table_model const& table) {
  table_config const* tcfg = table_config_for(cfg, table);
  if (tcfg == nullptr) {
    return to_pascal_case(table.name);
  }
  if (tcfg->skip) {
    return std::nullopt;
  }
  return tcfg->class_name ? *tcfg->class_name : to_pascal_case(table.name);
}

std::string join(std::vector<std::string> const& items,
  std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out += separator;
    }
    out += items[i];
  }
  return out;
}

// What one collision costs the run, and what can undo it. A class override
// works while the tables have distinct config keys; names differing by case
// alone share one key, so no section reaches one of them without the other.
std::string collision_report(std::string const& class_name,
  std::vector<std::string> const& tables) {
  std::map<std::string, std::vector<std::string>> by_key;
  for (std::string const& name : tables) {
    by_key[fold_lower(name)].push_back(name);
  }
  std::vector<std::string> twins;
  for (auto const& folded : by_key) {
    if (folded.second.size() > 1) {
      twins.push_back(join(folded.second, ", "));
    }
  }
  std::string out = "'" + class_name + "' names " + join(tables, ", ");
  if (twins.empty()) {
    return out + ". Set class in one of their [tables.*] sections.";
  }
  return out + ". " + join(twins, ", ") +
    " differ by case alone, which no [tables.*] section can split: ask for "
    "one of them with --tables.";
}

// Two tables reaching the same class name would declare one struct twice, and
// the header would not compile, so the run stops with the tables and the way
// out named.
void check_class_names(
  std::vector<std::pair<table_model const*, std::string>> const& emitted) {
  std::map<std::string, std::vector<std::string>> names;
  for (auto const& [table, class_name] : emitted) {
    names[class_name].push_back(table->name);
  }
  std::vector<std::string> collisions;
  for (auto const& [class_name, tables] : names) {
    if (tables.size() > 1) {
      collisions.push_back(collision_report(class_name, tables));
    }
  }
  if (collisions.empty()) {
    return;
  }
  throw config_error(
    "tables collide on one class name: " + join(collisions, "; "));
}

std::vector<member_info> build_members(
  table_model const& table, gen_config const& cfg, generated_output& out) {
  table_config const* tcfg = table_config_for(cfg, table);

  std::vector<member_info> members;
  std::unordered_set<std::string> used;
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    column_model const& col = table.columns[i];
    std::string where = table.name + "." + col.shape.name;

    column_override const* ovr = nullptr;
    if (tcfg != nullptr) {
      auto c_it = tcfg->columns.find(fold_lower(col.shape.name));
      if (c_it != tcfg->columns.end()) {
        ovr = &c_it->second;
      }
    }

    std::string cpp_type;
    bool through_converter = false;
    if (ovr != nullptr && ovr->converter) {
      // A converter override is a domain type by definition: the bindable
      // set does not apply, and the specialization is checked in the
      // generated header, where the type is complete.
      cpp_type = normalize_type(*ovr->converter);
      through_converter = true;
    } else if (ovr != nullptr && ovr->cpp_type) {
      cpp_type = check_bindable(*ovr->cpp_type, where);
    } else if (std::string const* global = find_type_override(col, cfg)) {
      cpp_type = check_bindable(*global, where);
    } else {
      cpp_type = default_cpp_type(col, where, out);
    }

    std::string member = to_camel_case(col.shape.name);
    if (used.count(member) != 0) {
      member += std::to_string(i);
      out.warnings.push_back(
        where + ": member name collision, renamed to '" + member + "'");
    }
    used.insert(member);
    members.push_back(
      member_info{ member, cpp_type, &col, through_converter });
  }
  return members;
}

void emit_table(std::string& text, table_model const& table,
  gen_config const& cfg, std::string const& class_name,
  generated_output& out) {
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
    if (m.col->shape.nullable) {
      decl = "std::optional<" + decl + ">";
    }
    text +=
      "  " + decl + " " + m.member + ";  // " + column_comment(*m.col) + "\n";
  }
  text += "};\n\n";

  std::unordered_set<std::string> asserted;
  for (member_info const& m : members) {
    if (!m.through_converter || !asserted.insert(m.cpp_type).second) {
      continue;
    }
    text += "static_assert(uniorm::has_converter<" + m.cpp_type +
            ">,\n  \"" + m.cpp_type +
            " needs a uniorm::converter specialization\");\n\n";
  }

  text += "inline void register_" + class_name +
          "_mapping(uniorm::orm& registry) {\n";
  text += "  registry.map<" + class_name + ">(\"" + table.name + "\")\n";
  for (std::size_t i = 0; i < members.size(); ++i) {
    member_info const& m = members[i];
    std::string call =
      m.col->primary_key ? "    .primary_key(" : "    .column(";
    call += "\"" + m.col->shape.name + "\", &" + class_name + "::" +
            m.member + ")";
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

  std::vector<std::pair<table_model const*, std::string>> emitted;
  for (table_model const& table : model.tables) {
    if (std::optional<std::string> class_name = emitted_class(cfg, table)) {
      emitted.emplace_back(&table, std::move(*class_name));
    }
  }
  check_class_names(emitted);

  for (auto const& [table, class_name] : emitted) {
    emit_table(text, *table, cfg, class_name, out);
  }

  text += "inline void register_" + unit + "_schema(uniorm::orm& registry) {\n";
  for (auto const& [table, class_name] : emitted) {
    text += "  register_" + class_name + "_mapping(registry);\n";
  }
  text += "}\n\n";
  text += "}  // namespace " + unit + "\n";
  return out;
}

void check_config(gen_config const& cfg, schema_model const& model,
  std::vector<std::string> const& catalog_tables) {
  std::unordered_set<std::string> listed;
  for (std::string const& name : catalog_tables) {
    listed.insert(fold_lower(name));
  }

  std::vector<std::string> offenders;
  for (auto const& [key, tcfg] : cfg.tables) {
    // Keys arrive folded, so a miss is a name the catalog does not have; the
    // printed form is folded with it.
    if (listed.count(key) == 0) {
      offenders.push_back("[tables." + key + "]");
      continue;
    }
    table_model const* table = nullptr;
    for (table_model const& m : model.tables) {
      if (fold_lower(m.name) == key) {
        table = &m;
        break;
      }
    }
    // A table this run left out reports no columns to check against.
    if (table == nullptr) {
      continue;
    }
    std::unordered_set<std::string> columns;
    for (column_model const& c : table->columns) {
      columns.insert(fold_lower(c.shape.name));
    }
    for (auto const& [column, ovr] : tcfg.columns) {
      if (columns.count(column) == 0) {
        offenders.push_back("[tables." + key + ".columns." + column + "]");
      }
    }
  }
  if (offenders.empty()) {
    return;
  }
  std::sort(offenders.begin(), offenders.end());
  throw config_error(
    "config names no table or column: " + join(offenders, ", "));
}

}  // namespace uniorm::gen
