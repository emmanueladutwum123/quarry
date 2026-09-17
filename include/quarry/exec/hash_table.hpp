// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The hash table underneath both grouping and joins.
///
/// Open addressing with linear probing, not chaining. A chained table costs a pointer
/// chase per probe into memory the prefetcher cannot predict, and at the sizes an
/// aggregation reaches -- millions of groups, far past any cache -- that chase *is*
/// the runtime. Linear probing keeps a collision inside the cache line that was
/// already fetched.
///
/// Keys are serialised into one arena rather than stored as typed tuples. A group key
/// of (int32, string) has no fixed size, and a table of variant tuples would allocate
/// per group and compare field by field. Serialised keys reduce both the hash and the
/// equality check to operations on a byte range, which is also what makes multi-column
/// grouping cost the same as single-column grouping.
///
/// The stored hash next to each slot is what makes the byte-range comparison rare:
/// two keys that land in the same slot almost always differ in their 64-bit hash, so
/// the memcmp only runs on a true match or a genuine collision.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace quarry {

/// 64-bit hash of a byte range. MurmurHash3's finaliser over 8-byte words: cheap,
/// and it avalanches well enough that the low bits are usable directly as a bucket
/// index. FNV-1a does not -- its low bits are poor, and a table masking them gets
/// clustering that looks like a workload problem.
inline std::uint64_t hash_bytes(const std::byte* data, std::size_t length) {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ (length * 0xFF51AFD7ED558CCDULL);
  std::size_t i = 0;
  for (; i + 8 <= length; i += 8) {
    std::uint64_t word = 0;
    std::memcpy(&word, data + i, 8);
    word *= 0x87C37B91114253D5ULL;
    word = (word << 31) | (word >> 33);
    h ^= word;
    h = ((h << 27) | (h >> 37)) * 5 + 0x52DCE729ULL;
  }
  std::uint64_t tail = 0;
  for (; i < length; ++i) {
    tail = (tail << 8) | static_cast<std::uint64_t>(data[i]);
  }
  h ^= tail * 0x87C37B91114253D5ULL;

  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ULL;
  h ^= h >> 33;
  return h;
}

/// Append-only storage for serialised keys. Offsets stay valid as it grows, which a
/// `vector<vector<byte>>` would also give but with one allocation per key.
class KeyArena {
 public:
  std::uint64_t append(const std::byte* data, std::uint32_t length) {
    const std::uint64_t at = bytes_.size();
    bytes_.insert(bytes_.end(), data, data + length);
    return at;
  }
  const std::byte* at(std::uint64_t offset) const { return bytes_.data() + offset; }
  std::size_t bytes() const { return bytes_.size(); }
  void clear() { bytes_.clear(); }

 private:
  std::vector<std::byte> bytes_;
};

/// Maps a serialised key to a dense payload index (0, 1, 2, ...), which callers use
/// to index their own parallel arrays of aggregate state or build-side row lists.
class GroupHashTable {
 public:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  GroupHashTable() { reset(1024); }

  /// Look up `key`, inserting it if absent. `inserted` reports which happened, so the
  /// caller knows whether to initialise new aggregate state.
  std::uint32_t find_or_insert(const std::byte* key, std::uint32_t length,
                               bool& inserted) {
    const std::uint64_t hash = hash_bytes(key, length);
    std::size_t slot = static_cast<std::size_t>(hash) & mask_;
    while (true) {
      Slot& entry = slots_[slot];
      if (entry.payload == kEmpty) {
        const std::uint64_t offset = arena_.append(key, length);
        entry.hash = hash;
        entry.key_offset = offset;
        entry.key_length = length;
        entry.payload = static_cast<std::uint32_t>(group_count_++);
        inserted = true;
        if (group_count_ * 10 >= slots_.size() * 7) grow();  // load factor 0.7
        return entry.payload;
      }
      if (entry.hash == hash && entry.key_length == length &&
          std::memcmp(arena_.at(entry.key_offset), key, length) == 0) {
        inserted = false;
        return entry.payload;
      }
      slot = (slot + 1) & mask_;
    }
  }

  /// Look up without inserting; returns kEmpty when absent.
  std::uint32_t find(const std::byte* key, std::uint32_t length) const {
    const std::uint64_t hash = hash_bytes(key, length);
    std::size_t slot = static_cast<std::size_t>(hash) & mask_;
    while (true) {
      const Slot& entry = slots_[slot];
      if (entry.payload == kEmpty) return kEmpty;
      if (entry.hash == hash && entry.key_length == length &&
          std::memcmp(arena_.at(entry.key_offset), key, length) == 0) {
        return entry.payload;
      }
      slot = (slot + 1) & mask_;
    }
  }

  std::size_t size() const { return group_count_; }

  /// The stored key for a payload index, for materialising group columns at the end.
  std::string_view key_of(std::uint32_t payload) const {
    for (const Slot& entry : slots_) {
      if (entry.payload == payload) {
        return std::string_view(
            reinterpret_cast<const char*>(arena_.at(entry.key_offset)),
            entry.key_length);
      }
    }
    return {};
  }

  /// Every (payload index, key) pair, in slot order. Used once at the end of an
  /// aggregation, which is why the linear scan above is acceptable only for lookups
  /// of a single key and this exists for the bulk case.
  std::vector<std::pair<std::uint32_t, std::string_view>> entries() const {
    std::vector<std::pair<std::uint32_t, std::string_view>> out;
    out.reserve(group_count_);
    for (const Slot& entry : slots_) {
      if (entry.payload == kEmpty) continue;
      out.emplace_back(entry.payload,
                       std::string_view(
                           reinterpret_cast<const char*>(arena_.at(entry.key_offset)),
                           entry.key_length));
    }
    return out;
  }

  void reset(std::size_t capacity) {
    std::size_t power = 16;
    while (power < capacity) power <<= 1;
    slots_.assign(power, Slot{});
    mask_ = power - 1;
    group_count_ = 0;
    arena_.clear();
  }

 private:
  struct Slot {
    std::uint64_t hash = 0;
    std::uint64_t key_offset = 0;
    std::uint32_t key_length = 0;
    std::uint32_t payload = kEmpty;
  };

  void grow() {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(old.size() * 2, Slot{});
    mask_ = slots_.size() - 1;
    for (const Slot& entry : old) {
      if (entry.payload == kEmpty) continue;
      std::size_t slot = static_cast<std::size_t>(entry.hash) & mask_;
      while (slots_[slot].payload != kEmpty) slot = (slot + 1) & mask_;
      slots_[slot] = entry;
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t group_count_ = 0;
  KeyArena arena_;
};

/// Serialise the key columns of one row into `out`.
///
/// Fixed-width values go in as their raw bytes; strings as a 4-byte length followed
/// by the characters. The length prefix is not decoration: without it, keys ("ab",
/// "c") and ("a", "bc") serialise identically and two different groups merge into
/// one. That bug produces plausible-looking wrong answers, which is the worst kind.
void serialize_key(const class Batch& batch, const std::vector<std::size_t>& key_columns,
                   std::size_t row, std::vector<std::byte>& out);

}  // namespace quarry
