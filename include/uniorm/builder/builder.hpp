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
#include <uniorm/detail/entity_binding.hpp>
#include <uniorm/detail/fetch_rows.hpp>
#include <uniorm/result_set.hpp>
#include <uniorm/row.hpp>

namespace uniorm {

class connection;

enum class direction { asc, desc };

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

// The chainable half of a typed read: the decoration, its rendering, and the
// synchronous read the terminals run. Shared by `query<T>` and the async
// builder so one SELECT is rendered one way; `Self` is the derived builder, so
// a chain keeps returning it and each face declares its own terminals.
template <class T, class Self>
class query_shape {
public:
  // A statement a terminal would run: the text, and the values its
  // placeholders bind in the order they appear.
  struct statement {
    std::string sql;
    std::vector<sql_value> bound;
  };

  Self& where(predicate p) {
    wheres_.push_back(std::move(p));
    return self();
  }

  template <class M>
  Self& order_by(M T::* member, direction dir = direction::asc) {
    orders_.push_back({ make_member_key(member), dir });
    return self();
  }

  Self& limit(std::size_t n) {
    limit_ = n;
    return self();
  }

  Self& offset(std::size_t n) {
    offset_ = n;
    return self();
  }

  std::string build_select() const {
    return render_select(limit_).sql;
  }

protected:
  query_shape(orm& db, entity_meta const& meta) : db_(&db), meta_(&meta) {}

  // The derived builder this decoration belongs to, so an asynchronous
  // terminal can carry a copy of it to the executor: the builder object the
  // call was made on is a temporary that dies at the end of that expression.
  Self& self() noexcept {
    return static_cast<Self&>(*this);
  }

  // `lim` overrides the chained limit; passing `limit_` renders the chain as
  // written.
  statement render_select(std::optional<std::size_t> lim) const {
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

    statement made{ std::move(sql), {} };
    if (!wheres_.empty()) {
      made.sql += " WHERE " + where_sql(resolve, made.bound);
    }
    if (!orders_.empty()) {
      made.sql += " ORDER BY ";
      for (std::size_t index = 0; index < orders_.size(); ++index) {
        if (index != 0) {
          made.sql += ", ";
        }
        made.sql += resolve(orders_[index].key);
        made.sql += orders_[index].dir == direction::asc ? " ASC" : " DESC";
      }
    }
    made.sql += dialect_ref.pagination(lim, offset_);
    return made;
  }

  statement render_count() const {
    statement made{
      "SELECT COUNT(*) FROM " + meta_->table_sql(sql_dialect()), {}
    };
    if (!wheres_.empty()) {
      made.sql += " WHERE " + where_sql(make_resolver(), made.bound);
    }
    return made;
  }

  std::vector<T> run_all() const {
    statement made = render_select(limit_);
    detail::entity_binding<T> binding(*meta_);
    params bound(std::move(made.bound));
    return detail::fetch_rows<T>(db_->native_connection(), made.sql, bound,
      db_->row_array_size(), binding);
  }

  std::optional<T> run_one() const {
    statement made = render_select(std::size_t{ 1 });
    detail::entity_binding<T> binding(*meta_);
    params bound(std::move(made.bound));
    return detail::fetch_one<T>(
      db_->native_connection(), made.sql, bound, binding);
  }

  std::int64_t run_count() const {
    statement made = render_count();
    params bound(std::move(made.bound));
    result_set rs = db_->native_connection().execute(made.sql, bound);
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

  orm* db_;
  entity_meta const* meta_;
  std::vector<predicate> wheres_;
  std::vector<order_clause> orders_;
  std::optional<std::size_t> limit_;
  std::size_t offset_ = 0;
};

// Read builder obtained via orm::query<T>(). Chained where() accumulates and
// ANDs; order_by/limit/offset decorate the SELECT; terminals are all/one/count.
template <class T>
class query : public query_shape<T, query<T>> {
  using shape = query_shape<T, query<T>>;

public:
  query(orm& db, entity_meta const& meta) : shape(db, meta) {}

  std::vector<T> all() {
    return this->run_all();
  }

  std::optional<T> one() {
    return this->run_one();
  }

  std::int64_t count() {
    return this->run_count();
  }
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
