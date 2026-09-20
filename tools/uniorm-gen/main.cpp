#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp"
#include "generator.hpp"
#include "naming.hpp"
#include "schema_reader.hpp"
#include "uniorm/error.hpp"
#include <uniorm/orm.hpp>

namespace {

char const* k_usage =
  "usage: uniorm-gen (--dsn=<dsn> [--user=<u> --password=<p>]\n"
  "                  | --connection-string=<str>)\n"
  "                  --out=<dir> [--config=<file>] [--tables=a,b,c]\n"
  "                  [--catalog=<c>] [--schema=<s>] [--name=<n>] [--help]\n"
  "\n"
  "Extracts the schema of a live database through uniorm's backend\n"
  "introspection and writes <out>/<name>_schema.hpp with entity structs\n"
  "and a register_<name>_schema(uniorm::orm&) function.\n"
  "\n"
  "--catalog, --schema and --tables name objects, they are not ODBC\n"
  "patterns: a name is matched for that name, case aside.\n";

bool read_flag(std::vector<std::string> const& args, std::string_view prefix,
  std::string& out) {
  for (std::string const& a : args) {
    if (a.size() > prefix.size() &&
        std::string_view(a).substr(0, prefix.size()) == prefix) {
      out = a.substr(prefix.size());
      return true;
    }
  }
  return false;
}

std::vector<std::string> split_csv(std::string const& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
}

std::string read_file(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw uniorm::uniorm_error("cannot open config file: " + path);
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  for (std::string const& a : args) {
    if (a == "--help" || a == "-h") {
      std::cout << k_usage;
      return 0;
    }
    if (a.rfind("--", 0) != 0) {
      std::cerr << "unexpected argument: " << a << "\n" << k_usage;
      return 2;
    }
  }

  std::string dsn, connection_string, user, password, out_dir, config_path,
    tables, catalog, schema, name;
  read_flag(args, "--dsn=", dsn);
  read_flag(args, "--connection-string=", connection_string);
  read_flag(args, "--user=", user);
  read_flag(args, "--password=", password);
  bool has_out = read_flag(args, "--out=", out_dir);
  read_flag(args, "--config=", config_path);
  read_flag(args, "--tables=", tables);
  read_flag(args, "--catalog=", catalog);
  read_flag(args, "--schema=", schema);
  read_flag(args, "--name=", name);

  if (dsn.empty() == connection_string.empty()) {
    std::cerr << "exactly one of --dsn / --connection-string is required\n"
              << k_usage;
    return 2;
  }
  if (!has_out || out_dir.empty()) {
    std::cerr << "--out=<dir> is required\n" << k_usage;
    return 2;
  }
  if (!dsn.empty()) {
    connection_string = "DSN=" + dsn;
    if (!user.empty()) {
      connection_string += ";UID=" + user;
    }
    if (!password.empty()) {
      connection_string += ";PWD=" + password;
    }
  }

  try {
    uniorm::gen::gen_config cfg;
    if (!config_path.empty()) {
      cfg = uniorm::gen::parse_config(read_file(config_path));
    }

    uniorm::orm db(connection_string);
    auto& md = db.schema();

    std::string unit = !name.empty() ? name : md.database_name();
    unit = uniorm::gen::to_unit_name(unit);
    if (unit == "_") {
      unit = "db";
    }

    uniorm::gen::read_options opts;
    opts.catalog = catalog;
    opts.schema = schema;
    opts.tables = split_csv(tables);

    std::vector<std::string> warnings;
    uniorm::gen::schema_model model =
      uniorm::gen::read_schema(md, opts, &warnings);
    model.name = unit;
    for (std::string const& w : warnings) {
      std::cerr << "warning: " << w << "\n";
    }

    // A section that matches nothing is an override silently ignored, so
    // every config table is checked against the catalog. That read lists the
    // whole catalog, so it happens only when there is a section to check.
    if (!cfg.tables.empty()) {
      std::vector<std::string> listed;
      for (auto const& row : md.tables(opts.catalog, opts.schema)) {
        listed.push_back(row.name);
      }
      uniorm::gen::check_config(cfg, model, listed);
    }

    uniorm::gen::generated_output out =
      uniorm::gen::generate_header(model, cfg);
    for (std::string const& w : out.warnings) {
      std::cerr << "warning: " << w << "\n";
    }

    std::filesystem::create_directories(out_dir);
    std::filesystem::path path =
      std::filesystem::path(out_dir) / (unit + "_schema.hpp");
    std::ofstream gen(path, std::ios::binary | std::ios::trunc);
    if (!gen) {
      throw uniorm::uniorm_error("cannot write " + path.string());
    }
    gen << out.text;
    gen.close();
    if (gen.fail()) {
      throw uniorm::uniorm_error("cannot write " + path.string());
    }
    std::cerr << "generated " << path.string() << " (" << model.tables.size()
              << " tables)\n";
    return 0;
  } catch (uniorm::uniorm_error const& e) {
    std::cerr << "uniorm-gen: " << e.what() << "\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "uniorm-gen: " << e.what() << "\n";
    return 1;
  }
}
