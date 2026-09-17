// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "quarry/types.hpp"

namespace quarry {

/// Thrown for a malformed file, an unknown column, or a type mismatch the planner
/// should have caught. A query engine that returns error codes from forty call sites
/// ends up checking none of them; these are all cases where the only sane response is
/// to abandon the query.
class QuarryError : public std::runtime_error {
 public:
  explicit QuarryError(const std::string& what) : std::runtime_error(what) {}
};

struct Field {
  std::string name;
  TypeId type = TypeId::Int32;
  bool nullable = false;
};

class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Field> fields) : fields_(std::move(fields)) {}

  std::size_t size() const { return fields_.size(); }
  const Field& operator[](std::size_t index) const { return fields_[index]; }
  const std::vector<Field>& fields() const { return fields_; }

  void add(std::string name, TypeId type, bool nullable = false) {
    fields_.push_back(Field{std::move(name), type, nullable});
  }

  /// Index of a column by name, or SIZE_MAX when absent. Callers that require the
  /// column use `index_of_or_throw` so the error names the column rather than
  /// surfacing later as an out-of-range crash.
  std::size_t index_of(const std::string& name) const {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
      if (fields_[i].name == name) return i;
    }
    return static_cast<std::size_t>(-1);
  }

  std::size_t index_of_or_throw(const std::string& name) const {
    const std::size_t index = index_of(name);
    if (index == static_cast<std::size_t>(-1)) {
      throw QuarryError("no such column: " + name);
    }
    return index;
  }

 private:
  std::vector<Field> fields_;
};

}  // namespace quarry
