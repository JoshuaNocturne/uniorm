#pragma once

// Entity-path builders: db.query<T>() / db.update<T>() / db.remove<T>().
// The dynamic (untyped-table) builders live alongside for a shared header.

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

// Fluent dynamic UPDATE for tables without an entity mapping, obtained via
// orm::update(table). Every set value is bound through a `?` placeholder;
// repeated where() calls accumulate and AND at execute(), matching the
// semantic of query<T>::where. execute() throws instead of firing a
// table-wide UPDATE.
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

  struct where_clause {
    std::string sql;
    params bound;
  };

  orm* orm_;
  std::string table_;
  std::vector<std::pair<std::string, sql_value>> set_;
  std::vector<where_clause> wheres_;
};

// Fluent dynamic DELETE, obtained via orm::remove(table). Repeated where()
// calls accumulate and AND at execute().
class UNIORM_API remove_builder {
public:
  remove_builder& where(std::string_view clause, params p = {});
  std::size_t execute();

private:
  friend class orm;
  remove_builder(orm& db, std::string table);

  struct where_clause {
    std::string sql;
    params bound;
  };

  orm* orm_;
  std::string table_;
  std::vector<where_clause> wheres_;
};

// Read builder obtained via orm::query<T>(). Chained where() accumulates and
// ANDs; order_by/limit/offset decorate the SELECT; terminals are all/one/count.
template <class T>
class query {
public:
  query(orm& db, entity_meta const& meta) : db_(&db), meta_(&meta) {}

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
    auto& c = db_->native_connection();
    std::string key(sql);
    auto stmt = c.acquire_statement(key);
    params p(std::move(bound));
    stmt->bind_params(p);
    stmt->execute();
    std::size_t ras = db_->row_array_size();
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
    auto& c = db_->native_connection();
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
    result_set rs = db_->native_connection().execute(
      sql, params(std::move(bound)));
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

  dialect const& sql_dialect() const {
    return db_->native_connection().sql_dialect();
  }

  predicate::resolver make_resolver() const {
    return [this](member_key const& key) {
      return meta_->column_sql(key, sql_dialect());
    };
  }

  std::string where_sql(
    predicate::resolver const& resolve, std::vector<sql_value>& bound) const {
    std::string sql;
    for (std::size_t index = 0; index < wheres_.size(); ++index) {
      if (index != 0) {
        sql += " AND ";
      }
      sql += wheres_[index].to_sql(resolve, bound);
    }
    return sql;
  }

  std::string render_select(
    std::optional<std::size_t> lim, std::vector<sql_value>& bound) const {
    auto const& dialect_ref = sql_dialect();
    auto resolve = make_resolver();

    std::string sql = "SELECT ";
    for (std::size_t index = 0; index < meta_->columns.size(); ++index) {
      if (index != 0) {
        sql += ", ";
      }
      sql += meta_->column_sql(index, dialect_ref);
    }
    sql += " FROM " + meta_->table_sql(dialect_ref);

    if (!wheres_.empty()) {
      sql += " WHERE " + where_sql(resolve, bound);
    }
    if (!orders_.empty()) {
      sql += " ORDER BY ";
      for (std::size_t index = 0; index < orders_.size(); ++index) {
        if (index != 0) {
          sql += ", ";
        }
        sql += resolve(orders_[index].key);
        sql += orders_[index].dir == direction::asc ? " ASC" : " DESC";
      }
    }
    sql += dialect_ref.pagination(lim, offset_);
    return sql;
  }

  std::string render_count(std::vector<sql_value>& bound) const {
    std::string sql =
      "SELECT COUNT(*) FROM " + meta_->table_sql(sql_dialect());
    if (!wheres_.empty()) {
      sql += " WHERE " + where_sql(make_resolver(), bound);
    }
    return sql;
  }

  orm* db_;
  entity_meta const* meta_;
  std::vector<predicate> wheres_;
  std::vector<order_clause> orders_;
  std::optional<std::size_t> limit_;
  std::size_t offset_ = 0;
};

// Typed UPDATE builder obtained via orm::update<T>(). Chained set() stages
// one column assignment each; chained where() accumulates and ANDs at
// execute(). execute() throws rather than firing a table-wide UPDATE.
template <class T>
class update {
public:
  update(orm& db, entity_meta const& meta) : db_(&db), meta_(&meta) {}

  // Stage a column assignment; nullptr writes NULL.
  template <class M, class V>
  update& set(M T::* member, V&& value) {
    sets_.emplace_back(
      make_member_key(member), detail::make_sql_value(std::forward<V>(value)));
    return *this;
  }

  update& where(predicate p) {
    wheres_.push_back(std::move(p));
    return *this;
  }

  std::size_t execute() {
    if (sets_.empty()) {
      throw uniorm_error("update: no columns to set");
    }
    if (wheres_.empty()) {
      throw uniorm_error("update: refusing to run without a WHERE predicate");
    }
    auto resolve = make_resolver();
    std::vector<sql_value> bound;
    bound.reserve(sets_.size());
    std::string sql = "UPDATE " + meta_->table_sql(sql_dialect()) + " SET ";
    for (std::size_t index = 0; index < sets_.size(); ++index) {
      if (index != 0) {
        sql += ", ";
      }
      sql += resolve(sets_[index].first) + " = ?";
      bound.push_back(sets_[index].second);
    }
    sql += " WHERE " + where_sql(resolve, bound);
    return db_->native_connection().execute_update(
      sql, params(std::move(bound)));
  }

private:
  dialect const& sql_dialect() const {
    return db_->native_connection().sql_dialect();
  }

  predicate::resolver make_resolver() const {
    return [this](member_key const& key) {
      return meta_->column_sql(key, sql_dialect());
    };
  }

  std::string where_sql(
    predicate::resolver const& resolve, std::vector<sql_value>& bound) const {
    std::string sql;
    for (std::size_t index = 0; index < wheres_.size(); ++index) {
      if (index != 0) {
        sql += " AND ";
      }
      sql += wheres_[index].to_sql(resolve, bound);
    }
    return sql;
  }

  orm* db_;
  entity_meta const* meta_;
  std::vector<std::pair<member_key, sql_value>> sets_;
  std::vector<predicate> wheres_;
};

// Typed DELETE builder obtained via orm::remove<T>(). Chained where()
// accumulates and ANDs at execute().
template <class T>
class remove {
public:
  remove(orm& db, entity_meta const& meta) : db_(&db), meta_(&meta) {}

  remove& where(predicate p) {
    wheres_.push_back(std::move(p));
    return *this;
  }

  std::size_t execute() {
    if (wheres_.empty()) {
      throw uniorm_error("remove: refusing to run without a WHERE predicate");
    }
    std::vector<sql_value> bound;
    std::string sql = "DELETE FROM " + meta_->table_sql(sql_dialect()) +
                      " WHERE " + where_sql(make_resolver(), bound);
    return db_->native_connection().execute_update(
      sql, params(std::move(bound)));
  }

private:
  dialect const& sql_dialect() const {
    return db_->native_connection().sql_dialect();
  }

  predicate::resolver make_resolver() const {
    return [this](member_key const& key) {
      return meta_->column_sql(key, sql_dialect());
    };
  }

  std::string where_sql(
    predicate::resolver const& resolve, std::vector<sql_value>& bound) const {
    std::string sql;
    for (std::size_t index = 0; index < wheres_.size(); ++index) {
      if (index != 0) {
        sql += " AND ";
      }
      sql += wheres_[index].to_sql(resolve, bound);
    }
    return sql;
  }

  orm* db_;
  entity_meta const* meta_;
  std::vector<predicate> wheres_;
};

// --- Builder factory methods on orm, out-of-class so that query<T> /
// update<T> / remove<T> are complete by the time their bodies instantiate.

template <class T>
inline ::uniorm::query<T> orm::query() {
  native_connection();
  return ::uniorm::query<T>(*this, meta<T>());
}

template <class T>
inline ::uniorm::update<T> orm::update() {
  native_connection();
  return ::uniorm::update<T>(*this, meta<T>());
}

template <class T>
inline ::uniorm::remove<T> orm::remove() {
  native_connection();
  return ::uniorm::remove<T>(*this, meta<T>());
}

}  // namespace uniorm
