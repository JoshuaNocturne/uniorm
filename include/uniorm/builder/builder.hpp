#pragma once

// Member-pointer query builder: conn.query(orm).of<T>()...

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <uniorm/dialect.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/params.hpp>
#include <uniorm/builder/expression.hpp>
#include <uniorm/result_set.hpp>
#include <uniorm/row.hpp>

namespace uniorm {

class connection;

namespace backend {
class statement_iface;
}

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

  void set_row_array_size(std::size_t size) {
    row_array_size_ = size;
  }

  void bind(backend::statement_iface& stmt) {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
      bindings_[i]->set_row_array_size(row_array_size_);
      bindings_[i]->bind(stmt, i + 1);
    }
  }

  T take(std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index, &proto_);
    }
    return std::move(proto_);
  }

  // Materialize one row directly into `dest` (vector storage). Bindings
  // write at captured member offsets, so no prototype round trip or
  // per-row move of small (SSO) strings is needed.
  void fill_into(T& dest, std::size_t row_index) {
    for (auto& binding : bindings_) {
      binding->finalize(row_index, &dest);
    }
  }

private:
  T proto_{};
  std::size_t row_array_size_ = 1;
  std::vector<std::unique_ptr<field_binding>> bindings_;
};

}  // namespace detail

template <class T>
class query;

// Fluent dynamic UPDATE for tables without an entity mapping, obtained via
// orm::update(table). Every set value is bound through a `?`
// placeholder; execute() throws instead of firing a table-wide UPDATE.
class UNIORM_API update_builder {
public:
  template <class V>
  update_builder& set(std::string_view column, V&& value) {
    set_.emplace_back(
      std::string(column), detail::make_sql_value(std::forward<V>(value)));
    return *this;
  }

  update_builder& where(std::string_view clause, params p = {});
  std::size_t execute();

private:
  friend class orm;
  update_builder(orm& db, std::string table);

  orm* orm_;
  std::string table_;
  std::vector<std::pair<std::string, sql_value>> set_;
  std::string where_;
  params where_params_;
};

// Fluent dynamic DELETE, obtained via orm::remove(table).
class UNIORM_API remove_builder {
public:
  remove_builder& where(std::string_view clause, params p = {});
  std::size_t execute();

private:
  friend class orm;
  remove_builder(orm& db, std::string table);

  orm* orm_;
  std::string table_;
  std::string where_;
  params where_params_;
};

// Entry point returned by orm::query(); owns nothing.
class UNIORM_API query_gateway {
public:
  explicit query_gateway(orm& db) : orm_(&db) {}

  template <class T>
  query<T> of() {
    return query<T>(*this, orm_->meta<T>());
  }

  connection& conn() const {
    return orm_->native_connection();
  }
  orm& get_orm() const {
    return *orm_;
  }
  std::size_t row_array_size() const noexcept {
    return orm_->row_array_size();
  }
  dialect const& sql_dialect() const;

private:
  orm* orm_;
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
    auto& c = gw_->conn();
    std::string key(sql);
    auto stmt = c.acquire_statement(key);
    params p(std::move(bound));
    stmt->bind_params(p);
    stmt->execute();
    std::size_t ras = gw_->row_array_size();
    stmt->set_row_array_size(ras);
    detail::entity_binding<T> binding(*meta_);
    binding.set_row_array_size(ras);
    binding.bind(*stmt);
    std::vector<T> out;
    if (std::size_t est = stmt->result_row_estimate()) out.reserve(est);
    while (stmt->fetch()) {
      std::size_t rows_fetched = stmt->rows_fetched();
      // Materialize rows directly into the vector's storage; the bindings
      // write at captured member offsets, avoiding per-row string moves.
      std::size_t old_size = out.size();
      out.resize(out.size() + rows_fetched);
      try {
        for (std::size_t i = 0; i < rows_fetched; ++i) {
          binding.fill_into(out[old_size + i], i);
        }
      } catch (...) {
        out.resize(old_size);
        throw;
      }
    }
    c.release_statement(key, std::move(stmt));
    return out;
  }

  std::optional<T> one() {
    std::vector<sql_value> bound;
    std::string sql = render_select(std::size_t{ 1 }, bound);
    auto& c = gw_->conn();
    std::string key(sql);
    auto stmt = c.acquire_statement(key);
    params p(std::move(bound));
    stmt->bind_params(p);
    stmt->execute();
    stmt->set_row_array_size(1);
    detail::entity_binding<T> binding(*meta_);
    binding.set_row_array_size(1);
    binding.bind(*stmt);
    std::optional<T> result;
    if (stmt->fetch()) {
      result = binding.take(0);
    }
    c.release_statement(key, std::move(stmt));
    return result;
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

  // Stage a column assignment for update(); nullptr writes NULL.
  template <class M, class V>
  query& set(M T::* member, V&& value) {
    sets_.emplace_back(
      make_member_key(member), detail::make_sql_value(std::forward<V>(value)));
    return *this;
  }

  // UPDATE ... SET <staged> WHERE <wheres>. Requires at least one set()
  // and one where(); order_by/limit/offset are ignored.
  std::size_t update() {
    if (sets_.empty()) {
      throw uniorm_error("update: no columns to set");
    }
    if (wheres_.empty()) {
      throw uniorm_error("update: refusing to run without a WHERE predicate");
    }
    auto resolve = make_resolver();
    std::vector<sql_value> bound;
    bound.reserve(sets_.size());
    std::string sql =
      "UPDATE " + gw_->sql_dialect().quote_identifier(meta_->table) + " SET ";
    for (std::size_t i = 0; i < sets_.size(); ++i) {
      if (i != 0) {
        sql += ", ";
      }
      sql += resolve(sets_[i].first) + " = ?";
      bound.push_back(sets_[i].second);
    }
    sql += " WHERE " + where_sql(resolve, bound);
    return gw_->conn().execute_update(sql, params(std::move(bound)));
  }

  // DELETE FROM ... WHERE <wheres>. Requires at least one where();
  // order_by/limit/offset are ignored.
  std::size_t remove() {
    if (wheres_.empty()) {
      throw uniorm_error("remove: refusing to run without a WHERE predicate");
    }
    std::vector<sql_value> bound;
    std::string sql = "DELETE FROM " +
                      gw_->sql_dialect().quote_identifier(meta_->table) +
                      " WHERE " + where_sql(make_resolver(), bound);
    return gw_->conn().execute_update(sql, params(std::move(bound)));
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
  std::vector<std::pair<member_key, sql_value>> sets_;
  std::vector<order_clause> orders_;
  std::optional<std::size_t> limit_;
  std::size_t offset_ = 0;
};

}  // namespace uniorm
