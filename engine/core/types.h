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

// MurmurHash3 finalizer ("fmix64"). Near-perfect avalanche: each input bit
// flips every output bit with probability ~0.5.
//
// Used inside Hasher::combine to avoid structural hash collisions (where
// similar inputs produce similar outputs). BUG-026 postmortem: the old
// boost::hash_combine-style `seed_ ^= v + golden + shifts` had poor
// avalanche — Love Letter 2p hand sequences collided with some public
// sequences at rate ~0.01%/sim, triggering DAG node misuse. fmix64 fixes
// this by injecting full avalanche per combine step.
inline std::uint64_t murmur3_fmix64(std::uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

// Accumulator for building a StateHash64 from game-defined fields. Used by
// IGameState::hash_public_fields / hash_private_fields so the framework can
// structurally derive `state_hash_for_perspective(p) = hash(public + priv_p)`.
// Games call `combine(v)` once per field they want included.
class Hasher {
 public:
  void combine(std::uint64_t v) {
    // BUG-026 fix: use fmix64-based mix instead of boost::hash_combine.
    // Ensures structurally-similar states don't produce near-equal hashes.
    // Non-commutative (order-dependent), preserving the ordering guarantees
    // callers rely on (e.g. discard pile replay order).
    seed_ = murmur3_fmix64(seed_ ^ (v + kGoldenRatio64));
  }
  // Convenience for integral / enum / small POD types. Silently truncates to
  // 64 bits; if your value is wider than uint64_t, combine its halves manually.
  template <typename T>
  void add(const T& v) {
    combine(static_cast<std::uint64_t>(v));
  }
  // Combine a byte range. Useful for fixed-size arrays.
  void combine_bytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t acc = 0;
    std::size_t shift = 0;
    for (std::size_t i = 0; i < n; ++i) {
      acc |= static_cast<std::uint64_t>(p[i]) << (shift * 8U);
      shift = (shift + 1U) % 8U;
      if (shift == 0) { combine(acc); acc = 0; }
    }
    if (shift != 0) combine(acc);
  }
  StateHash64 finalize() const { return seed_; }

 private:
  std::uint64_t seed_ = 0;
};

struct UndoToken {
  std::uint32_t undo_depth = 0;
};

}  // namespace board_ai
