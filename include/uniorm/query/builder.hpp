#pragma once

// Member-pointer query builder: conn.query(orm).of<T>()...

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <uniorm/backend/backend.hpp>
#include <uniorm/connection.hpp>
#include <uniorm/dialect.hpp>
#include <uniorm/mapping/registry.hpp>
#include <uniorm/params.hpp>
#include <uniorm/result_set.hpp>
#include <uniorm/row.hpp>
#include <uniorm/query/expression.hpp>

namespace uniorm {

enum class direction { asc, desc };

namespace detail {

// Binds all mapped columns directly onto the fields of one entity instance.
// Relies on the SELECT projecting columns in meta.columns registration order.
template <class T>
class entity_binding {
public:
  explicit entity_binding(entity_meta const& meta) {
    bindings_.reserve(meta.columns.size());
    for (auto const& c : meta.columns) {
      bindings_.push_back(c.make_binding(&proto_));
    }
  }

  void bind(backend::statement_iface& stmt) {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
      bindings_[i]->bind(stmt, i + 1);
    }
  }

  T take() {
    for (auto& binding : bindings_) {
      binding->finalize();
    }
    return std::move(proto_);
  }

private:
  T proto_{};
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace detail

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
    return gw_->conn().execute_with(
      sql, params(std::move(bound)), [this](backend::statement_iface& stmt) {
        detail::entity_binding<T> binding(*meta_);
        binding.bind(stmt);
        std::vector<T> out;
        while (stmt.fetch()) {
          out.push_back(binding.take());
        }
        return out;
      });
  }

  std::optional<T> one() {
    std::vector<sql_value> bound;
    std::string sql = render_select(std::size_t{ 1 }, bound);
    return gw_->conn().execute_with(sql, params(std::move(bound)),
      [this](backend::statement_iface& stmt) -> std::optional<T> {
        detail::entity_binding<T> binding(*meta_);
        binding.bind(stmt);
        if (!stmt.fetch()) {
          return std::nullopt;
        }
        return binding.take();
      });
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
