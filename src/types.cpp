// SPDX-License-Identifier: Apache-2.0
#include "quarry/types.hpp"

#include <cstdio>

namespace quarry {

std::string Value::to_string() const {
  char buffer[64];
  switch (type) {
    case TypeId::Int32:
      std::snprintf(buffer, sizeof(buffer), "%d", i32);
      return buffer;
    case TypeId::Int64:
      std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(i64));
      return buffer;
    case TypeId::Double:
      std::snprintf(buffer, sizeof(buffer), "%.17g", f64);
      return buffer;
    case TypeId::String:
      return str;
  }
  return {};
}

}  // namespace quarry
