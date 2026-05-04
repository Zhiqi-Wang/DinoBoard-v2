#include "azul_state.h"

#include <algorithm>
#include <functional>

namespace board_ai::azul {

template <int NPlayers>
AzulState<NPlayers>::AzulState() {
  reset_with_seed(0xC0FFEEu);
}

template <int NPlayers>
void AzulState<NPlayers>::reset_with_seed(std::uint64_t seed) {
  this->step_count_ = 0;
  current_player_ = 0;
  game_first_player_ = 0;
  first_player_next_round = 0;
  winner_ = -1;
  round_index = 0;
  terminal = false;
  first_player_token_in_center = true;
  shared_victory = false;
  scores = {};
  factories = {};
  center = {};
  bag.clear();
  box_lid.clear();
  players = {};
  undo_stack.clear();
  persistent_tree_cache.tree.clear();
  persistent_tree_cache.chance_buckets.clear();
  persistent_tree_cache.sig_to_node.clear();
  rng_salt = sanitize_seed(seed);
  bag.reserve(100);
  for (int c = 0; c < kColors; ++c) {
    for (int i = 0; i < 20; ++i) {
      bag.push_back(static_cast<std::int8_t>(c));
    }
  }
  for (int i = static_cast<int>(bag.size()) - 1; i > 0; --i) {
    const int j = static_cast<int>(next_rand_u32() % static_cast<std::uint32_t>(i + 1));
    std::swap(bag[static_cast<size_t>(i)], bag[static_cast<size_t>(j)]);
  }
  refill_factories_from_rng();
}

template <int NPlayers>
bool AzulState<NPlayers>::all_sources_empty() const {
  for (const auto& fac : factories) {
    for (std::uint8_t c : fac) {
      if (c > 0) {
        return false;
      }
    }
  }
  for (std::uint8_t c : center) {
    if (c > 0) {
      return false;
    }
  }
  return !first_player_token_in_center;
}

template <int NPlayers>
std::uint32_t AzulState<NPlayers>::next_rand_u32() {
  std::uint64_t x = rng_salt;
  x ^= x >> 12U;
  x ^= x << 25U;
  x ^= x >> 27U;
  rng_salt = x;
  return static_cast<std::uint32_t>((x * 2685821657736338717ULL) >> 32U);
}

template <int NPlayers>
int AzulState<NPlayers>::draw_one_tile() {
  if (bag.empty()) {
    if (box_lid.empty()) {
      return -1;
    }
    bag.assign(box_lid.begin(), box_lid.end());
    box_lid.clear();
    for (int i = static_cast<int>(bag.size()) - 1; i > 0; --i) {
      const int j = static_cast<int>(next_rand_u32() % static_cast<std::uint32_t>(i + 1));
      std::swap(bag[static_cast<size_t>(i)], bag[static_cast<size_t>(j)]);
    }
  }
  const int t = bag.back();
  bag.pop_back();
  return t;
}

template <int NPlayers>
void AzulState<NPlayers>::refill_factories_from_rng() {
  factories = {};
  center = {};
  for (int f = 0; f < Cfg::kFactories; ++f) {
    for (int i = 0; i < 4; ++i) {
      const int color = draw_one_tile();
      if (color < 0 || color >= kColors) {
        continue;
      }
      factories[f][color] = static_cast<std::uint8_t>(factories[f][color] + 1);
    }
  }
}

template <int NPlayers>
bool AzulState<NPlayers>::is_tree_cache_consistent() const {
  if (persistent_tree_cache.tree.size() < persistent_tree_cache.chance_buckets.size()) {
    return false;
  }
  for (const auto& kv : persistent_tree_cache.sig_to_node) {
    const int idx = kv.second;
    if (idx < 0 || idx >= static_cast<int>(persistent_tree_cache.tree.size())) {
      return false;
    }
  }
  return true;
}

template <int NPlayers>
StateHash64 AzulState<NPlayers>::state_hash(bool include_hidden_rng) const {
  std::size_t h = 0;
  hash_combine(h,static_cast<std::size_t>(current_player_));
  hash_combine(h,static_cast<std::size_t>(first_player_next_round));
  hash_combine(h,static_cast<std::size_t>(winner_ + 1));
  hash_combine(h,static_cast<std::size_t>(round_index));
  hash_combine(h,static_cast<std::size_t>(terminal ? 1 : 0));
  hash_combine(h,static_cast<std::size_t>(first_player_token_in_center ? 1 : 0));
  hash_combine(h,static_cast<std::size_t>(shared_victory ? 1 : 0));
  for (int s : scores) {
    hash_combine(h,static_cast<std::size_t>(s));
  }
  for (const auto& fac : factories) {
    for (std::uint8_t c : fac) {
      hash_combine(h,static_cast<std::size_t>(c));
    }
  }
  for (std::uint8_t c : center) {
    hash_combine(h,static_cast<std::size_t>(c));
  }
  hash_combine(h,static_cast<std::size_t>(bag.size()));
  for (std::int8_t t : bag) {
    hash_combine(h,static_cast<std::size_t>(t + 1));
  }
  hash_combine(h,static_cast<std::size_t>(box_lid.size()));
  for (std::int8_t t : box_lid) {
    hash_combine(h,static_cast<std::size_t>(t + 1));
  }
  for (const auto& p : players) {
    for (std::uint8_t len : p.line_len) {
      hash_combine(h,static_cast<std::size_t>(len));
    }
    for (std::int8_t color : p.line_color) {
      hash_combine(h,static_cast<std::size_t>(color + 1));
    }
    for (std::uint8_t m : p.wall_mask) {
      hash_combine(h,static_cast<std::size_t>(m));
    }
    hash_combine(h,static_cast<std::size_t>(p.floor_count));
    for (std::int8_t f : p.floor) {
      hash_combine(h,static_cast<std::size_t>(f + 1));
    }
    hash_combine(h,static_cast<std::size_t>(p.score));
  }
  if (include_hidden_rng) {
    hash_combine(h,static_cast<std::size_t>(rng_salt));
  }
  return static_cast<StateHash64>(h);
}

template <int NPlayers>
void AzulState<NPlayers>::hash_public_fields(Hasher& h) const {
  // Azul is symmetric-random but fully PUBLIC with respect to composition:
  // everyone sees factory contents, center pile, each player's board, and
  // knows the bag composition derivably (bag = all tiles − placed − discarded).
  // The only thing nobody knows is the future draw ORDER, which is not a
  // field in state — it's randomness resolved at sample_is_world time.
  h.add(current_player_);
  h.add(first_player_next_round);
  h.add(winner_ + 1);
  h.add(round_index);
  h.add(terminal ? 1 : 0);
  h.add(first_player_token_in_center ? 1 : 0);
  h.add(shared_victory ? 1 : 0);
  for (int s : scores) h.add(s);
  for (const auto& fac : factories) {
    for (std::uint8_t c : fac) h.add(c);
  }
  for (std::uint8_t c : center) h.add(c);
  h.add(bag.size());
  for (std::int8_t t : bag) h.add(t + 1);
  h.add(box_lid.size());
  for (std::int8_t t : box_lid) h.add(t + 1);
  for (const auto& p : players) {
    for (std::uint8_t len : p.line_len) h.add(len);
    for (std::int8_t color : p.line_color) h.add(color + 1);
    for (std::uint8_t m : p.wall_mask) h.add(m);
    h.add(p.floor_count);
    for (std::int8_t f : p.floor) h.add(f + 1);
    h.add(p.score);
  }
}

template <int NPlayers>
void AzulState<NPlayers>::hash_private_fields(int /*player*/, Hasher& /*h*/) const {
  // Azul has no non-symmetric private info. Bag composition is
  // deterministically derivable from public placements; only order is random.
}

template class AzulState<2>;
template class AzulState<3>;
template class AzulState<4>;

}  // namespace board_ai::azul
