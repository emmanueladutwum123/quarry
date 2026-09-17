// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A test harness in one header, for the same reason the engine has no third-party
/// dependencies: a query engine that cannot be built from a bare checkout and a
/// compiler is a query engine nobody runs.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace quarry::test {

inline std::string to_text(const std::string& value) { return "\"" + value + "\""; }
inline std::string to_text(const char* value) {
  return value ? "\"" + std::string(value) + "\"" : "(null)";
}
inline std::string to_text(bool value) { return value ? "true" : "false"; }

template <typename T>
inline std::string to_text(const T& value) {
  if constexpr (std::is_enum<T>::value) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_arithmetic<T>::value) {
    // Promote the narrow character types: to_string has no overload for them and
    // would otherwise be ambiguous.
    if constexpr (sizeof(T) == 1) {
      return std::to_string(static_cast<long long>(value));
    } else {
      return std::to_string(value);
    }
  } else {
    return "<unprintable>";
  }
}

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);
int run_all(const char* suite);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace quarry::test

#define TEST(name)                                                  \
  static void name();                                               \
  static ::quarry::test::Registrar name##_registrar(#name, &name);  \
  static void name()

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) ::quarry::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    const auto _lhs = (a);                                                \
    const auto _rhs = (b);                                                \
    if (!(_lhs == _rhs)) {                                                \
      ::quarry::test::fail(__FILE__, __LINE__,                            \
                           std::string("CHECK_EQ(" #a ", " #b ")  lhs=") + \
                               ::quarry::test::to_text(_lhs) +            \
                               " rhs=" + ::quarry::test::to_text(_rhs));  \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                     \
  do {                                                                            \
    const double _lhs = static_cast<double>(a);                                   \
    const double _rhs = static_cast<double>(b);                                   \
    const double _diff = _lhs > _rhs ? _lhs - _rhs : _rhs - _lhs;                 \
    if (!(_diff <= static_cast<double>(tol))) {                                   \
      ::quarry::test::fail(__FILE__, __LINE__,                                    \
                           std::string("CHECK_NEAR(" #a ", " #b ")  lhs=") +      \
                               std::to_string(_lhs) + " rhs=" +                   \
                               std::to_string(_rhs));                             \
    }                                                                             \
  } while (0)
