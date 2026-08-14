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
#include "uniorm/odbc/connection.hpp"
#include "uniorm/odbc/environment.hpp"

namespace {

char const* k_usage =
  "usage: uniorm-gen (--dsn=<dsn> [--user=<u> --password=<p>]\n"
  "                  | --connection-string=<str>)\n"
  "                  --out=<dir> [--config=<file>] [--tables=a,b,c]\n"
  "                  [--catalog=<c>] [--schema=<s>] [--name=<n>] [--help]\n"
  "\n"
  "Extracts the schema of a live database through ODBC metadata and\n"
  "writes <out>/<name>_schema.hpp with entity structs and a\n"
  "register_<name>_schema(uniorm::orm&) function.\n";

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

  std::string dsn, conn_string, user, password, out_dir, config_path, tables,
    catalog, schema, name;
  read_flag(args, "--dsn=", dsn);
  read_flag(args, "--connection-string=", conn_string);
  read_flag(args, "--user=", user);
  read_flag(args, "--password=", password);
  bool has_out = read_flag(args, "--out=", out_dir);
  read_flag(args, "--config=", config_path);
  read_flag(args, "--tables=", tables);
  read_flag(args, "--catalog=", catalog);
  read_flag(args, "--schema=", schema);
  read_flag(args, "--name=", name);

  if (dsn.empty() == conn_string.empty()) {
    std::cerr << "exactly one of --dsn / --connection-string is required\n"
              << k_usage;
    return 2;
  }
  if (!has_out || out_dir.empty()) {
    std::cerr << "--out=<dir> is required\n" << k_usage;
    return 2;
  }

  try {
    uniorm::gen::gen_config cfg;
    if (!config_path.empty()) {
      cfg = uniorm::gen::parse_config(read_file(config_path));
    }

    uniorm::odbc::connection conn(uniorm::odbc::shared_environment());
    if (!dsn.empty()) {
      conn.open_dsn(dsn, user, password);
    } else {
      conn.open(conn_string);
    }

    std::string unit = !name.empty() ? name : uniorm::gen::database_name(conn);
    unit = uniorm::gen::to_unit_name(unit);
    if (unit == "_") {
      unit = "db";
    }

    uniorm::gen::read_options opts;
    if (!catalog.empty()) {
      opts.catalog = catalog;
    }
    if (!schema.empty()) {
      opts.schema = schema;
    }
    opts.tables = split_csv(tables);

    std::vector<std::string> warnings;
    uniorm::gen::schema_model model =
      uniorm::gen::read_schema(conn, opts, &warnings);
    model.name = unit;

    uniorm::gen::generated_output out =
      uniorm::gen::generate_header(model, cfg);
    for (std::string const& w : warnings) {
      std::cerr << "warning: " << w << "\n";
    }
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
