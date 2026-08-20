#include "uniorm/query/builder.hpp"

namespace uniorm {

dialect const& query_gateway::sql_dialect() const {
  if (!dialect_detected_) {
    dialect_ = uniorm::dialect::detect(orm_->dbms_name());
    dialect_detected_ = true;
  }
  return dialect_;
}

}  // namespace uniorm
