#pragma once

// Private header: OCI handle and descriptor RAII wrappers. Not installed.
// OCI frees no child handle when its parent goes and frees no descriptor
// at all, so every wrapper here carries the kind it must be released as.

#include <string>
#include <utility>
#include <vector>

#include <oci.h>

#include "error.hpp"

namespace uniorm::oracle_oci::detail {

// Move-only owner of an OCI handle. Statements, errors and describes are
// allocated against the environment; bind and define handles are the
// exception, since OCIBindByPos and OCIDefineByPos allocate those, so they
// arrive through adopt().
template <class Handle, ub4 HandleType>
class basic_handle {
public:
  basic_handle() = default;

  basic_handle(OCIEnv* parent, char const* what) {
    void* raw = nullptr;
    if (OCIHandleAlloc(parent, &raw, HandleType, 0, nullptr) != OCI_SUCCESS ||
        raw == nullptr) {
      throw oci_error("allocate the OCI handle for " + std::string(what), {});
    }
    handle_ = static_cast<Handle*>(raw);
  }

  static basic_handle adopt(Handle* handle) noexcept {
    basic_handle owned;
    owned.handle_ = handle;
    return owned;
  }

  ~basic_handle() {
    reset();
  }

  basic_handle(basic_handle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

  basic_handle& operator=(basic_handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  basic_handle(basic_handle const&) = delete;
  basic_handle& operator=(basic_handle const&) = delete;

  Handle* get() const noexcept {
    return handle_;
  }
  explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

  void reset() noexcept {
    if (handle_ != nullptr) {
      OCIHandleFree(handle_, HandleType);
      handle_ = nullptr;
    }
  }

private:
  Handle* handle_ = nullptr;
};

using error_handle = basic_handle<OCIError, OCI_HTYPE_ERROR>;
using stmt_handle = basic_handle<OCIStmt, OCI_HTYPE_STMT>;
using bind_handle = basic_handle<OCIBind, OCI_HTYPE_BIND>;
using define_handle = basic_handle<OCIDefine, OCI_HTYPE_DEFINE>;
// Each OCIParamGet on a statement hands out a fresh descriptor, so a select
// list walked without this leaks one per column per describe.
using param_handle = basic_handle<OCIParam, OCI_DTYPE_PARAM>;

// Move-only owner of an OCI descriptor: the LOB locators and date/time
// values a define or bind points at. The parent is the environment for a
// locator allocated before a session, and the service context otherwise;
// either is accepted here.
template <class Descriptor, ub4 DescriptorType>
class basic_descriptor {
public:
  basic_descriptor() = default;

  explicit basic_descriptor(void const* parent, char const* what) {
    void* raw = nullptr;
    if (OCIDescriptorAlloc(
          parent, &raw, DescriptorType, 0, nullptr) != OCI_SUCCESS ||
        raw == nullptr) {
      throw oci_error(
        "allocate the OCI descriptor for " + std::string(what), {});
    }
    descriptor_ = static_cast<Descriptor*>(raw);
  }

  ~basic_descriptor() {
    reset();
  }

  basic_descriptor(basic_descriptor&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, nullptr)) {}

  basic_descriptor& operator=(basic_descriptor&& other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, nullptr);
    }
    return *this;
  }

  basic_descriptor(basic_descriptor const&) = delete;
  basic_descriptor& operator=(basic_descriptor const&) = delete;

  Descriptor* get() const noexcept {
    return descriptor_;
  }
  // Where the pointer itself lives. A datetime or LOB bind and an array
  // define are given this, not get(): OCI reads a pointer to the descriptor,
  // and handing it the descriptor is a crash inside the wire encoder.
  void* address() noexcept {
    return &descriptor_;
  }
  explicit operator bool() const noexcept {
    return descriptor_ != nullptr;
  }

  void reset() noexcept {
    if (descriptor_ != nullptr) {
      OCIDescriptorFree(descriptor_, DescriptorType);
      descriptor_ = nullptr;
    }
  }

private:
  Descriptor* descriptor_ = nullptr;
};

using lob_descriptor = basic_descriptor<OCILobLocator, OCI_DTYPE_LOB>;
using timestamp_descriptor = basic_descriptor<OCIDateTime, OCI_DTYPE_TIMESTAMP>;
using timestamp_tz_descriptor =
  basic_descriptor<OCIDateTime, OCI_DTYPE_TIMESTAMP_TZ>;
using timestamp_ltz_descriptor =
  basic_descriptor<OCIDateTime, OCI_DTYPE_TIMESTAMP_LTZ>;

// The datetime descriptors a column's fetched rows point at. A TIMESTAMP, a
// TIMESTAMP WITH TIME ZONE and a TIMESTAMP WITH LOCAL TIME ZONE are three
// allocation kinds of one struct, and a define whose descriptor is of the
// other kind reads back garbage, so each pointer carries its own kind to be
// freed as.
class datetime_staging {
public:
  datetime_staging() = default;
  ~datetime_staging() {
    for (std::size_t row = 0; row < values_.size(); ++row) {
      if (values_[row] != nullptr) {
        OCIDescriptorFree(values_[row], kinds_[row]);
      }
    }
  }

  datetime_staging(datetime_staging&&) = default;
  datetime_staging(datetime_staging const&) = delete;
  datetime_staging& operator=(datetime_staging const&) = delete;
  datetime_staging& operator=(datetime_staging&&) = delete;

  // One more descriptor, of `kind`, appended and returned.
  OCIDateTime* add(void const* parent, ub4 kind, char const* what) {
    void* raw = nullptr;
    if (OCIDescriptorAlloc(parent, &raw, kind, 0, nullptr) != OCI_SUCCESS ||
        raw == nullptr) {
      throw oci_error(
        "allocate the OCI descriptor for " + std::string(what), {});
    }
    values_.push_back(static_cast<OCIDateTime*>(raw));
    kinds_.push_back(kind);
    return values_.back();
  }

  OCIDateTime* at(std::size_t row) const noexcept {
    return values_[row];
  }
  OCIDateTime** data() noexcept {
    return values_.data();
  }
  std::size_t size() const noexcept {
    return values_.size();
  }

private:
  std::vector<OCIDateTime*> values_;
  std::vector<ub4> kinds_;
};

}  // namespace uniorm::oracle_oci::detail
