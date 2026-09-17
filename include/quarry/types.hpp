// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The type system, kept deliberately small.
///
/// Four logical types cover every TPC-H column once dates are stored as days since
/// epoch and decimals as scaled integers. A wider type system is not more impressive;
/// it is more code between a bug and the test that would have caught it. Each type
/// added here has to earn its place in the encoder, the zone map, the comparison
/// kernels and the hash table -- four places, not one.

#include <cstdint>
#include <string>
#include <string_view>

namespace quarry {

enum class TypeId : std::uint8_t {
  Int32 = 0,   ///< also carries DATE, as days since 1970-01-01
  Int64 = 1,   ///< also carries DECIMAL, as a value scaled by a fixed factor
  Double = 2,
  String = 3,
};

/// Fixed width in bytes, or 0 for the variable-length types.
constexpr std::size_t type_width(TypeId type) {
  switch (type) {
    case TypeId::Int32: return 4;
    case TypeId::Int64: return 8;
    case TypeId::Double: return 8;
    case TypeId::String: return 0;
  }
  return 0;
}

constexpr bool is_fixed_width(TypeId type) { return type_width(type) != 0; }

constexpr std::string_view type_name(TypeId type) {
  switch (type) {
    case TypeId::Int32: return "INT32";
    case TypeId::Int64: return "INT64";
    case TypeId::Double: return "DOUBLE";
    case TypeId::String: return "STRING";
  }
  return "?";
}

/// A single scalar, used for literals in predicates and for zone-map bounds.
///
/// Not a `std::variant`: the engine compares millions of these against column values
/// and a variant visit at that rate shows up in a profile. The tagged union keeps the
/// comparison a load and a branch that the predicate loop hoists out entirely.
struct Value {
  TypeId type = TypeId::Int32;
  union {
    std::int32_t i32;
    std::int64_t i64;
    double f64;
  };
  std::string str;  ///< only meaningful when `type == String`

  Value() : i64(0) {}

  static Value of_int32(std::int32_t v) {
    Value out;
    out.type = TypeId::Int32;
    out.i32 = v;
    return out;
  }
  static Value of_int64(std::int64_t v) {
    Value out;
    out.type = TypeId::Int64;
    out.i64 = v;
    return out;
  }
  static Value of_double(double v) {
    Value out;
    out.type = TypeId::Double;
    out.f64 = v;
    return out;
  }
  static Value of_string(std::string v) {
    Value out;
    out.type = TypeId::String;
    out.i64 = 0;
    out.str = std::move(v);
    return out;
  }

  /// Three-way comparison within a type. Comparing across types is a planner bug, not
  /// a runtime condition, so it is not represented here.
  int compare(const Value& other) const {
    switch (type) {
      case TypeId::Int32: return (i32 < other.i32) ? -1 : (i32 > other.i32) ? 1 : 0;
      case TypeId::Int64: return (i64 < other.i64) ? -1 : (i64 > other.i64) ? 1 : 0;
      case TypeId::Double: return (f64 < other.f64) ? -1 : (f64 > other.f64) ? 1 : 0;
      case TypeId::String: return str.compare(other.str) < 0 ? -1
                                : (str.compare(other.str) > 0 ? 1 : 0);
    }
    return 0;
  }

  bool operator==(const Value& other) const {
    return type == other.type && compare(other) == 0;
  }

  std::string to_string() const;
};

}  // namespace quarry
