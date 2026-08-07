#pragma once

#include <stdexcept>

#include "export.hpp"

namespace uniorm {

class UNIORM_API uniorm_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class UNIORM_API unicode_error : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

class UNIORM_API column_not_found : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

class UNIORM_API type_mismatch : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

class UNIORM_API mapping_error : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

class UNIORM_API pool_timeout : public uniorm_error {
public:
  using uniorm_error::uniorm_error;
};

}  // namespace uniorm
