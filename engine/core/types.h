#pragma once

#include <cstdint>
#include <cstddef>

namespace board_ai {

using ActionId = std::int32_t;
using StateHash64 = std::uint64_t;

constexpr std::uint64_t kGoldenRatio64 = 0x9e3779b97f4a7c15ULL;

inline void hash_combine(std::size_t& seed, std::size_t v) {
  seed ^= v + kGoldenRatio64 + (seed << 6U) + (seed >> 2U);
}

inline std::uint64_t splitmix64(std::uint64_t& x) {
  x += kGoldenRatio64;
  std::uint64_t z = x;
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

inline std::uint64_t sanitize_seed(std::uint64_t seed) {
  return seed == 0 ? kGoldenRatio64 : seed;
}

struct UndoToken {
  std::uint32_t undo_depth = 0;
};

}  // namespace board_ai
