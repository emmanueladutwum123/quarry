// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A read-only memory mapping of a segment file.
///
/// The scan reads encoded bytes straight out of the mapping: no read() into a buffer,
/// no copy before decode. For a column the query does not touch, the pages are never
/// faulted in at all -- which is most of the benefit of a columnar layout, and it
/// only materialises if the reader avoids a bulk read of the whole file.

#include <cstddef>
#include <string>

namespace quarry {

class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  /// Throws QuarryError when the file cannot be opened or mapped.
  static MappedFile open(const std::string& path);

  const std::byte* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool valid() const { return data_ != nullptr; }

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace quarry
