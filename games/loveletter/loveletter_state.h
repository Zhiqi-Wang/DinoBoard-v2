#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../engine/core/game_interfaces.h"

namespace board_ai::loveletter {

constexpr std::int8_t kGuard = 1;
constexpr std::int8_t kPriest = 2;
constexpr std::int8_t kBaron = 3;
constexpr std::int8_t kHandmaid = 4;
constexpr std::int8_t kPrince = 5;
constexpr std::int8_t kKing = 6;
constexpr std::int8_t kCountess = 7;
constexpr std::int8_t kPrincess = 8;

constexpr int kCardTypes = 8;
constexpr int kTotalCards = 16;
constexpr std::array<int, 9> kCardCounts = {0, 5, 2, 2, 2, 2, 1, 1, 1};

constexpr int kGuardOffset = 0;
constexpr int kGuardCount = 28;       // target(0..3) * 7 + (guess-2)
constexpr int kPriestOffset = 28;
constexpr int kPriestCount = 4;
constexpr int kBaronOffset = 32;
constexpr int kBaronCount = 4;
constexpr int kHandmaidAction = 36;
constexpr int kPrinceOffset = 37;
constexpr int kPrinceCount = 4;       // includes self-target
constexpr int kKingOffset = 41;
constexpr int kKingCount = 4;
constexpr int kCountessAction = 45;
constexpr int kPrincessAction = 46;
constexpr int kActionSpace = 47;

template <int NPlayers>
struct LoveLetterConfig {
  static_assert(NPlayers >= 2 && NPlayers <= 4);
  static constexpr int kPlayers = NPlayers;
  static constexpr int kPerPlayerFeatures = 29;
  static constexpr int kGlobalFeatures = 12;
  static constexpr int kFeatureDim = kPerPlayerFeatures * NPlayers + kGlobalFeatures;
  static constexpr int kFaceUpRemoved = NPlayers == 2 ? 3 : 0;
};

template <int NPlayers>
struct LoveLetterData {
  using Cfg = LoveLetterConfig<NPlayers>;

  int current_player = 0;
  std::int8_t first_player = 0;
  int winner = -1;
  bool terminal = false;
  int ply = 0;

  std::array<std::int8_t, Cfg::kPlayers> hand{};
  std::int8_t drawn_card = 0;

  std::array<std::int8_t, Cfg::kPlayers> alive{};
  std::array<std::int8_t, Cfg::kPlayers> protected_flags{};
  std::array<std::int8_t, Cfg::kPlayers> hand_exposed{};

  std::vector<std::int8_t> deck;
  std::int8_t set_aside_card = 0;
  std::array<std::vector<std::int8_t>, Cfg::kPlayers> discard_piles;
  std::vector<std::int8_t> face_up_removed;

  std::uint64_t draw_nonce = 0;
};

template <int NPlayers>
struct LoveLetterState final : public CloneableState<LoveLetterState<NPlayers>> {
  using Cfg = LoveLetterConfig<NPlayers>;
  LoveLetterData<NPlayers> data;
  std::uint64_t rng_salt = 0;
  std::vector<LoveLetterData<NPlayers>> undo_stack;

  LoveLetterState();
  void reset_with_seed(std::uint64_t seed) override;

  StateHash64 state_hash(bool include_hidden_rng) const override;
  int current_player() const override;
  int first_player() const override;
  bool is_terminal() const override;
  int num_players() const override { return Cfg::kPlayers; }
  int winner() const override;
  std::uint64_t rng_nonce() const override;
};

extern template struct LoveLetterData<2>;
extern template struct LoveLetterData<3>;
extern template struct LoveLetterData<4>;
extern template struct LoveLetterState<2>;
extern template struct LoveLetterState<3>;
extern template struct LoveLetterState<4>;

}  // namespace board_ai::loveletter
