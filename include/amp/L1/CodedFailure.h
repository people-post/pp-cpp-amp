#pragma once

#include "common/Error.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace pp::adp {

/** Layer-local coded failure — `Err` enum is defined by the owning class. */
template <typename ErrEnum>
struct CodedFailure : pp::RoeErrorBase {
  static CodedFailure Of(const ErrEnum e, std::string detail) {
    CodedFailure failure;
    failure.code = static_cast<int32_t>(e);
    failure.category = 0;
    failure.message = std::move(detail);
    return failure;
  }

  ErrEnum GetCode() const { return static_cast<ErrEnum>(code); }
};

template <typename T, typename ErrEnum>
using CodedRoe = pp::ResultOrError<T, CodedFailure<ErrEnum>>;

namespace detail {

inline std::string AppendFrom(std::string detail, const std::string_view from_label, const std::string& from_message) {
  if (from_message.empty()) {
    return detail;
  }
  detail += " [";
  detail.append(from_label.begin(), from_label.end());
  detail += ": ";
  detail += from_message;
  detail += "]";
  return detail;
}

} // namespace detail

} // namespace pp::adp
