#include "uniorm/query/expression.hpp"

#include <utility>

#include "uniorm/error.hpp"

namespace uniorm {

struct predicate::node {
  enum class kind {
    comparison,
    in_list,
    null_check,
    like,
    conjunction,
    disjunction
  };

  kind k = kind::comparison;
  member_key key{ std::type_index(typeid(void)), {} };
  std::string op;
  std::vector<sql_value> values;
  bool negated = false;
  std::vector<predicate> children;
};

predicate predicate::comparison(
  member_key key, std::string_view op, sql_value value) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::comparison;
  n->key = std::move(key);
  n->op = std::string(op);
  n->values.push_back(std::move(value));
  p.node_ = std::move(n);
  return p;
}

predicate predicate::in_list(member_key key, std::vector<sql_value> values) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::in_list;
  n->key = std::move(key);
  n->values = std::move(values);
  p.node_ = std::move(n);
  return p;
}

predicate predicate::null_check(member_key key, bool negated) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::null_check;
  n->key = std::move(key);
  n->negated = negated;
  p.node_ = std::move(n);
  return p;
}

predicate predicate::like_expr(member_key key, std::string pattern) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::like;
  n->key = std::move(key);
  n->values.push_back(sql_value(std::move(pattern)));
  p.node_ = std::move(n);
  return p;
}

predicate predicate::conjunction(predicate lhs, predicate rhs) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::conjunction;
  n->children.push_back(std::move(lhs));
  n->children.push_back(std::move(rhs));
  p.node_ = std::move(n);
  return p;
}

predicate predicate::disjunction(predicate lhs, predicate rhs) {
  predicate p;
  auto n = std::make_shared<node>();
  n->k = node::kind::disjunction;
  n->children.push_back(std::move(lhs));
  n->children.push_back(std::move(rhs));
  p.node_ = std::move(n);
  return p;
}

std::string predicate::to_sql(
  resolver const& resolve, std::vector<sql_value>& out) const {
  if (!node_) {
    throw uniorm_error("empty predicate cannot be rendered");
  }
  switch (node_->k) {
  case node::kind::comparison:
    out.push_back(node_->values.front());
    return resolve(node_->key) + " " + node_->op + " ?";
  case node::kind::in_list: {
    if (node_->values.empty()) {
      return "1 = 0";
    }
    std::string sql = resolve(node_->key) + " IN (";
    for (std::size_t i = 0; i < node_->values.size(); ++i) {
      if (i != 0) {
        sql += ", ";
      }
      sql += "?";
    }
    sql += ")";
    for (auto const& v : node_->values) {
      out.push_back(v);
    }
    return sql;
  }
  case node::kind::null_check:
    return resolve(node_->key) + (node_->negated ? " IS NOT NULL" : " IS NULL");
  case node::kind::like:
    out.push_back(node_->values.front());
    return resolve(node_->key) + " LIKE ?";
  case node::kind::conjunction:
  case node::kind::disjunction: {
    char const* joiner = node_->k == node::kind::conjunction ? " AND " : " OR ";
    std::string sql = "(";
    for (std::size_t i = 0; i < node_->children.size(); ++i) {
      if (i != 0) {
        sql += joiner;
      }
      sql += node_->children[i].to_sql(resolve, out);
    }
    sql += ")";
    return sql;
  }
  }
  throw uniorm_error("unknown predicate kind");
}

}  // namespace uniorm
