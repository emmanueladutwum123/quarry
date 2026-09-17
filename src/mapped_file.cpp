// SPDX-License-Identifier: Apache-2.0
#include "quarry/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "quarry/schema.hpp"

namespace quarry {

MappedFile::~MappedFile() {
  if (data_ != nullptr) {
    ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
  }
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    if (data_ != nullptr) {
      ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
    }
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

MappedFile MappedFile::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    throw QuarryError("open " + path + ": " + std::strerror(errno));
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    throw QuarryError("fstat " + path + ": " + message);
  }
  const auto bytes = static_cast<std::size_t>(info.st_size);
  if (bytes == 0) {
    ::close(fd);
    throw QuarryError("empty segment file: " + path);
  }

  void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
  // The mapping outlives the descriptor: mmap holds its own reference to the file, so
  // closing here avoids leaking a descriptor per open segment. A query touching a
  // thousand partitions would otherwise run out of them.
  ::close(fd);
  if (address == MAP_FAILED) {
    throw QuarryError("mmap " + path + ": " + std::strerror(errno));
  }

  MappedFile file;
  file.data_ = static_cast<const std::byte*>(address);
  file.size_ = bytes;
  return file;
}

}  // namespace quarry
