// SPDX-License-Identifier: Apache-2.0
#include "quarry/bitpack.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace quarry;

namespace {

/// Pack then unpack and require the values back exactly. Run for every width from 0
/// to 64 rather than a handful: the bugs in a bit packer live at the boundaries --
/// where a value straddles a byte, where the last value runs past the end of the
/// buffer, where the mask overflows -- and those are different widths each time.
void roundtrip_at_width(std::uint32_t width, std::size_t count) {
  std::mt19937_64 rng(width * 1000 + count);
  const std::uint64_t limit =
      (width >= 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);

  std::vector<std::uint64_t> input(count);
  for (auto& value : input) value = limit == 0 ? 0 : (rng() % (limit + (limit < ~std::uint64_t{0} ? 1 : 0)));

  std::vector<std::byte> buffer(bitpack::packed_size(count, width) + 8, std::byte{0xAB});
  bitpack::pack(input.data(), count, width, buffer.data());

  std::vector<std::uint64_t> output(count, ~std::uint64_t{0});
  bitpack::unpack(buffer.data(), count, width, output.data());

  for (std::size_t i = 0; i < count; ++i) {
    if (input[i] != output[i]) {
      ::quarry::test::fail(__FILE__, __LINE__,
                           "width=" + std::to_string(width) + " i=" + std::to_string(i) +
                               " in=" + std::to_string(input[i]) +
                               " out=" + std::to_string(output[i]));
      return;
    }
  }
}

}  // namespace

TEST(bitpack_roundtrips_at_every_width) {
  for (std::uint32_t width = 0; width <= 64; ++width) {
    roundtrip_at_width(width, 97);  // a prime count, so no width divides it evenly
  }
}

TEST(bitpack_roundtrips_odd_counts) {
  for (std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8},
                            std::size_t{63}, std::size_t{64}, std::size_t{65},
                            std::size_t{1024}}) {
    for (std::uint32_t width : {1u, 3u, 7u, 8u, 13u, 17u, 32u, 57u, 64u}) {
      roundtrip_at_width(width, count);
    }
  }
}

TEST(width_for_is_the_minimum_that_fits) {
  CHECK_EQ(bitpack::width_for(0), 0u);
  CHECK_EQ(bitpack::width_for(1), 1u);
  CHECK_EQ(bitpack::width_for(2), 2u);
  CHECK_EQ(bitpack::width_for(255), 8u);
  CHECK_EQ(bitpack::width_for(256), 9u);
  CHECK_EQ(bitpack::width_for(~std::uint64_t{0}), 64u);
}

TEST(packed_size_matches_the_bits_written) {
  CHECK_EQ(bitpack::packed_size(0, 7), std::size_t{0});
  CHECK_EQ(bitpack::packed_size(8, 1), std::size_t{1});
  CHECK_EQ(bitpack::packed_size(9, 1), std::size_t{2});
  CHECK_EQ(bitpack::packed_size(100, 0), std::size_t{0});
  CHECK_EQ(bitpack::packed_size(3, 17), std::size_t{7});  // 51 bits -> 7 bytes
}

TEST(zero_width_costs_nothing_and_reads_back_as_zero) {
  // A column of one repeated value: the value lives in the zone map, the payload is
  // empty, and the scan still has to produce `count` rows.
  std::vector<std::byte> buffer(8, std::byte{0xFF});
  std::vector<std::uint64_t> output(16, 7);
  bitpack::unpack(buffer.data(), output.size(), 0, output.data());
  for (std::uint64_t value : output) CHECK_EQ(value, std::uint64_t{0});
}

TEST(packing_does_not_write_past_the_computed_size) {
  // The guard bytes after `packed_size` must survive: an off-by-one here corrupts the
  // next column chunk in the file, which is the worst kind of bug to find later.
  const std::size_t count = 37;
  const std::uint32_t width = 13;
  const std::size_t size = bitpack::packed_size(count, width);

  std::vector<std::uint64_t> input(count);
  for (std::size_t i = 0; i < count; ++i) input[i] = (i * 601) & 0x1FFF;

  std::vector<std::byte> buffer(size + 16, std::byte{0x5A});
  bitpack::pack(input.data(), count, width, buffer.data());
  for (std::size_t i = size; i < buffer.size(); ++i) {
    CHECK_EQ(static_cast<int>(buffer[i]), 0x5A);
  }
}
