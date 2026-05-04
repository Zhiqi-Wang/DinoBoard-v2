#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../../engine/core/game_interfaces.h"

namespace board_ai::azul {

constexpr int kRows = 5;
constexpr int kColors = 5;
constexpr int kTargetsPerColor = 6;  // line[0..4] + floor

template <int NPlayers>
struct AzulConfig {
  static_assert(NPlayers >= 2 && NPlayers <= 4);
  static constexpr int kPlayers = NPlayers;
  static constexpr int kFactories = 2 * NPlayers + 1;
  static constexpr int kCenterSource = kFactories;
  static constexpr int kActionSpace = (kFactories + 1) * kColors * kTargetsPerColor;
  static constexpr int kFeatureDim = NPlayers * 61 + NPlayers + kFactories * kColors + kColors + kColors + 4;
};

struct PlayerState {
  std::array<std::uint8_t, kRows> line_len{};
  std::array<std::int8_t, kRows> line_color{{-1, -1, -1, -1, -1}};
  std::array<std::uint8_t, kRows> wall_mask{};
  std::array<std::int8_t, 7> floor{{-1, -1, -1, -1, -1, -1, -1}};
  std::uint8_t floor_count = 0;
  int score = 0;
};

template <int NPlayers>
struct AzulSnapshot {
  using Cfg = AzulConfig<NPlayers>;
  int current_player = 0;
  int first_player_next_round = 0;
  int winner = -1;
  int round_index = 0;
  bool terminal = false;
  bool first_player_token_in_center = true;
  bool shared_victory = false;
  std::array<int, Cfg::kPlayers> scores{};
  std::array<std::array<std::uint8_t, kColors>, Cfg::kFactories> factories{};
  std::array<std::uint8_t, kColors> center{};
  std::vector<std::int8_t> bag{};
  std::vector<std::int8_t> box_lid{};
  std::array<PlayerState, Cfg::kPlayers> players{};
  std::uint64_t rng_salt = 0;
};

template <int NPlayers>
struct UndoRecord {
  using Cfg = AzulConfig<NPlayers>;
  bool use_full_snapshot = false;
  AzulSnapshot<NPlayers> full_before{};
  int prev_current_player = 0;
  int prev_first_player_next_round = 0;
  int prev_winner = -1;
  int prev_round_index = 0;
  bool prev_terminal = false;
  bool prev_first_player_token_in_center = true;
  bool prev_shared_victory = false;
  std::array<int, Cfg::kPlayers> prev_scores{};
  std::array<std::uint8_t, kColors> prev_center{};
  bool has_factory_source = false;
  int source_factory_idx = -1;
  std::array<std::uint8_t, kColors> prev_factory_source{};
  PlayerState prev_player{};
  std::uint64_t prev_rng_salt = 0;
};

struct PersistentTreeCache {
  std::vector<int> tree{};
  std::vector<int> chance_buckets{};
  std::unordered_map<StateHash64, int> sig_to_node{};
};

template <int NPlayers>
class AzulState final : public CloneableState<AzulState<NPlayers>> {
 public:
  using Cfg = AzulConfig<NPlayers>;

  AzulState();

  StateHash64 state_hash(bool include_hidden_rng) const override;
  void hash_public_fields(Hasher& h) const override;
  void hash_private_fields(int player, Hasher& h) const override;
  int current_player() const override { return current_player_; }
  int first_player() const override { return game_first_player_; }
  bool is_terminal() const override { return terminal; }
  int num_players() const override { return Cfg::kPlayers; }
  int winner() const override { return winner_; }

  int current_player_ = 0;
  int game_first_player_ = 0;
  int first_player_next_round = 0;
  int winner_ = -1;
  int round_index = 0;
  bool terminal = false;
  bool first_player_token_in_center = true;
  bool shared_victory = false;
  std::array<int, Cfg::kPlayers> scores{};

  std::array<std::array<std::uint8_t, kColors>, Cfg::kFactories> factories{};
  std::array<std::uint8_t, kColors> center{};
  std::vector<std::int8_t> bag{};
  std::vector<std::int8_t> box_lid{};
  std::array<PlayerState, Cfg::kPlayers> players{};

  std::vector<UndoRecord<NPlayers>> undo_stack{};
  PersistentTreeCache persistent_tree_cache{};
  std::uint64_t rng_salt = 0;

  void reset_with_seed(std::uint64_t seed) override;
  bool all_sources_empty() const;
  void refill_factories_from_rng();
  int draw_one_tile();
  std::uint32_t next_rand_u32();
  StateHash64 state_signature() const { return state_hash(false); }
  bool is_tree_cache_consistent() const;
};

extern template class AzulState<2>;
extern template class AzulState<3>;
extern template class AzulState<4>;

}  // namespace board_ai::azul
