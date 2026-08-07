#pragma once

// Member-pointer query builder: conn.query(orm).of<T>()...

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../connection.hpp"
#include "../dialect.hpp"
#include "../mapping/registry.hpp"
#include "../params.hpp"
#include "../result_set.hpp"
#include "../row.hpp"
#include "expression.hpp"

namespace uniorm {

enum class direction { asc, desc };

template <class T>
class query;

// Entry point returned by connection::query(orm); owns nothing.
class UNIORM_API query_gateway {
public:
  query_gateway(connection& conn, orm& registry)
    : conn_(&conn), registry_(&registry) {}

  template <class T>
  query<T> of() {
    return query<T>(*this, registry_->meta<T>());
  }

  connection& conn() const {
    return *conn_;
  }
  orm& registry() const {
    return *registry_;
  }
  dialect const& sql_dialect() const;

private:
  connection* conn_;
  orm* registry_;
  mutable bool dialect_detected_ = false;
  mutable dialect dialect_;
};

template <class T>
class query {
public:
  query(query_gateway& gw, entity_meta const& meta) : gw_(&gw), meta_(&meta) {}

  query& where(predicate p) {
    wheres_.push_back(std::move(p));
    return *this;
  }

  template <class M>
  query& order_by(M T::* member, direction dir = direction::asc) {
    orders_.push_back({ make_member_key(member), dir });
    return *this;
  }

  query& limit(std::size_t n) {
    limit_ = n;
    return *this;
  }

  query& offset(std::size_t n) {
    offset_ = n;
    return *this;
  }

  std::string build_select() const {
    std::vector<sql_value> discard;
    return render_select(limit_, discard);
  }

  std::vector<T> all() {
    std::vector<sql_value> bound;
    std::string sql = render_select(limit_, bound);
    result_set rs = gw_->conn().execute(sql, params(std::move(bound)));
    std::vector<T> out;
    while (rs.next()) {
      row r = rs.current();
      T obj{};
      meta_->populate(&obj, r);
      out.push_back(std::move(obj));
    }
    return out;
  }

  std::optional<T> one() {
    std::vector<sql_value> bound;
    std::string sql = render_select(std::size_t{ 1 }, bound);
    result_set rs = gw_->conn().execute(sql, params(std::move(bound)));
    if (!rs.next()) {
      return std::nullopt;
    }
    row r = rs.current();
    T obj{};
    meta_->populate(&obj, r);
    return obj;
  }

  std::int64_t count() {
    std::vector<sql_value> bound;
    std::string sql = render_count(bound);
    result_set rs = gw_->conn().execute(sql, params(std::move(bound)));
    if (!rs.next()) {
      return 0;
    }
    return rs.current().get<std::int64_t>(0);
  }

private:
  struct order_clause {
    member_key key;
    direction dir;
  };

  predicate::resolver make_resolver() const {
    return [this](member_key const& key) {
      return gw_->sql_dialect().quote_identifier(meta_->column_name(key));
    };
  }

  std::string where_sql(
    predicate::resolver const& resolve, std::vector<sql_value>& bound) const {
    std::string sql;
    for (std::size_t i = 0; i < wheres_.size(); ++i) {
      if (i != 0) {
        sql += " AND ";
      }
      sql += wheres_[i].to_sql(resolve, bound);
    }
    return sql;
  }

  std::string render_select(
    std::optional<std::size_t> lim, std::vector<sql_value>& bound) const {
    auto const& d = gw_->sql_dialect();
    auto resolve = make_resolver();

    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < meta_->columns.size(); ++i) {
      if (i != 0) {
        sql += ", ";
      }
      sql += d.quote_identifier(meta_->columns[i].column);
    }
    sql += " FROM " + d.quote_identifier(meta_->table);

    if (!wheres_.empty()) {
      sql += " WHERE " + where_sql(resolve, bound);
    }
    if (!orders_.empty()) {
      sql += " ORDER BY ";
      for (std::size_t i = 0; i < orders_.size(); ++i) {
        if (i != 0) {
          sql += ", ";
        }
        sql += resolve(orders_[i].key);
        sql += orders_[i].dir == direction::asc ? " ASC" : " DESC";
      }
    }
    sql += d.pagination(lim, offset_);
    return sql;
  }

  std::string render_count(std::vector<sql_value>& bound) const {
    auto const& d = gw_->sql_dialect();
    std::string sql =
      "SELECT COUNT(*) FROM " + d.quote_identifier(meta_->table);
    if (!wheres_.empty()) {
      sql += " WHERE " + where_sql(make_resolver(), bound);
    }
    return sql;
  }

  query_gateway* gw_;
  entity_meta const* meta_;
  std::vector<predicate> wheres_;
  std::vector<order_clause> orders_;
  std::optional<std::size_t> limit_;
  std::size_t offset_ = 0;
};

}  // namespace uniorm
