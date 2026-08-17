#include <uniorm/backend/registry.hpp>

#include <algorithm>
#include <cctype>

#include <uniorm/backend/error.hpp>

namespace uniorm::backend {

namespace {

bool valid_scheme_char(char c, bool first) {
  if (first) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
  }
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' ||
         c == '.' || c == '-';
}

}  // namespace

parsed_scheme parse_scheme(std::string_view cs) {
  std::size_t sep = cs.find("://");
  if (sep == std::string_view::npos) {
    return {"odbc", cs};
  }
  std::string_view candidate = cs.substr(0, sep);
  if (candidate.empty() ||
      !valid_scheme_char(candidate.front(), true)) {
    return {"odbc", cs};
  }
  for (char c : candidate.substr(1)) {
    if (!valid_scheme_char(c, false)) {
      return {"odbc", cs};
    }
  }
  std::string scheme(candidate);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return {std::move(scheme), cs.substr(sep + 3)};
}

registry& registry::instance() {
  static registry r;
  return r;
}

void registry::register_backend(std::string scheme, factory f) {
  std::lock_guard lock(mu_);
  if (backends_.count(scheme) != 0) {
    throw backend_error(scheme, "backend scheme already registered", {});
  }
  backends_.emplace(std::move(scheme), std::move(f));
}

bool registry::contains(std::string_view scheme) const {
  std::lock_guard lock(mu_);
  return backends_.count(std::string(scheme)) != 0;
}

std::vector<std::string> registry::schemes() const {
  std::lock_guard lock(mu_);
  std::vector<std::string> out;
  out.reserve(backends_.size());
  for (auto const& [scheme, _] : backends_) {
    out.push_back(scheme);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::unique_ptr<connection_iface> registry::create(
  std::string_view connection_string, std::string* tail) const {
  parsed_scheme parsed = parse_scheme(connection_string);
  factory make;
  {
    std::lock_guard lock(mu_);
    auto it = backends_.find(parsed.scheme);
    if (it != backends_.end()) {
      make = it->second;
    }
  }
  if (!make) {
    std::string msg =
      "no backend registered for scheme '" + parsed.scheme +
      "' (registered:";
    for (auto const& s : schemes()) {
      msg += " " + s;
    }
    msg += ")";
    throw unknown_scheme(msg);
  }
  if (tail != nullptr) {
    *tail = std::string(parsed.tail);
  }
  return make();
}

registrar::registrar(std::string scheme, registry::factory f) {
  registry::instance().register_backend(std::move(scheme), std::move(f));
}

}  // namespace uniorm::backend
