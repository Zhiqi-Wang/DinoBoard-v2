#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "../../engine/core/game_interfaces.h"

namespace board_ai::splendor {

constexpr int kColorCount = 5;
constexpr int kTokenTypes = 6;
constexpr int kTargetPoints = 15;
constexpr int kMaxPlies = 160;

template <int NPlayers>
struct SplendorConfig {
  static_assert(NPlayers >= 2 && NPlayers <= 4);
  static constexpr int kPlayers = NPlayers;
  static constexpr int kGemCount = NPlayers == 2 ? 4 : (NPlayers == 3 ? 5 : 7);
  static constexpr int kGoldCount = 5;
  static constexpr int kNobleCount = NPlayers + 1;

  static constexpr int kBuyFaceupOffset = 0;
  static constexpr int kBuyFaceupCount = 12;
  static constexpr int kReserveFaceupOffset = kBuyFaceupOffset + kBuyFaceupCount;
  static constexpr int kReserveFaceupCount = 12;
  static constexpr int kReserveDeckOffset = kReserveFaceupOffset + kReserveFaceupCount;
  static constexpr int kReserveDeckCount = 3;
  static constexpr int kBuyReservedOffset = kReserveDeckOffset + kReserveDeckCount;
  static constexpr int kBuyReservedCount = 3;
  static constexpr int kTakeThreeOffset = kBuyReservedOffset + kBuyReservedCount;
  static constexpr int kTakeThreeCount = 10;
  static constexpr int kTakeTwoDifferentOffset = kTakeThreeOffset + kTakeThreeCount;
  static constexpr int kTakeTwoDifferentCount = 10;
  static constexpr int kTakeOneOffset = kTakeTwoDifferentOffset + kTakeTwoDifferentCount;
  static constexpr int kTakeOneCount = 5;
  static constexpr int kTakeTwoSameOffset = kTakeOneOffset + kTakeOneCount;
  static constexpr int kTakeTwoSameCount = 5;
  static constexpr int kChooseNobleOffset = kTakeTwoSameOffset + kTakeTwoSameCount;
  static constexpr int kChooseNobleCount = kNobleCount;
  static constexpr int kReturnTokenOffset = kChooseNobleOffset + kChooseNobleCount;
  static constexpr int kReturnTokenCount = 6;
  static constexpr int kPassAction = kReturnTokenOffset + kReturnTokenCount;
  static constexpr int kActionSpace = kPassAction + 1;

  static constexpr int kPerPlayerFeatures = 15;  // gems(6)+bonuses(5)+points(1)+reserved(1)+cards(1)+nobles(1)
  static constexpr int kPerPlayerReserves = 3 * 13;  // 3 slots * 13 features per card
  static constexpr int kFeatureDim = 6 + kPerPlayerFeatures * kPlayers +
      kNobleCount * 6 + 12 * 13 + kPerPlayerReserves * kPlayers + 7;
};

enum class SplendorTurnStage : std::int8_t {
  kNormal = 0,
  kReturnTokens = 1,
  kChooseNoble = 2,
};

struct SplendorCard {
  std::int8_t tier = 1;
  std::int8_t bonus = 0;
  std::int8_t points = 0;
  std::array<std::int8_t, kColorCount> cost{};
};

template <int NPlayers>
struct SplendorData {
  using Cfg = SplendorConfig<NPlayers>;
  int current_player = 0;
  std::int8_t first_player = 0;
  int plies = 0;
  int final_round_remaining = -1;
  std::int8_t stage = static_cast<std::int8_t>(SplendorTurnStage::kNormal);
  int pending_returns = 0;
  std::array<std::int8_t, Cfg::kNobleCount> pending_noble_slots{};
  std::int8_t pending_nobles_size = 0;
  std::uint64_t draw_nonce = 0;
  std::int16_t forced_draw_override = -1;
  int winner = -1;
  bool terminal = false;
  bool shared_victory = false;
  std::array<int, Cfg::kPlayers> scores{};

  std::array<std::int8_t, kTokenTypes> bank{};
  std::array<std::array<std::int8_t, kTokenTypes>, Cfg::kPlayers> player_gems{};
  std::array<std::array<std::int8_t, kColorCount>, Cfg::kPlayers> player_bonuses{};
  std::array<std::int16_t, Cfg::kPlayers> player_points{};
  std::array<std::int16_t, Cfg::kPlayers> player_cards_count{};
  std::array<std::int16_t, Cfg::kPlayers> player_nobles_count{};

  std::array<std::array<std::int16_t, 3>, Cfg::kPlayers> reserved{};
  std::array<std::array<std::int8_t, 3>, Cfg::kPlayers> reserved_visible{};
  std::array<std::int8_t, Cfg::kPlayers> reserved_size{};

  std::array<std::vector<std::int16_t>, 3> decks{};
  std::array<std::array<std::int16_t, 4>, 3> tableau{};
  std::array<std::int8_t, 3> tableau_size{};

  std::array<std::int16_t, Cfg::kNobleCount> nobles{};
  std::int8_t nobles_size = 0;
};

template <int NPlayers>
struct SplendorPersistentNode {
  std::shared_ptr<const SplendorPersistentNode<NPlayers>> parent{};
  ActionId action_from_parent = -1;
  mutable std::shared_ptr<const SplendorData<NPlayers>> materialized{};
};

template <int NPlayers>
class SplendorPersistentState {
 public:
  SplendorPersistentState() = default;
  explicit SplendorPersistentState(std::shared_ptr<const SplendorPersistentNode<NPlayers>> node);

  static SplendorPersistentState root_from_seed(std::uint64_t seed);

  bool valid() const { return static_cast<bool>(node_); }
  const SplendorData<NPlayers>& data() const;
  SplendorPersistentState advance(ActionId action) const;
  StateHash64 state_hash(bool include_hidden_rng, std::uint64_t rng_salt) const;

 private:
  std::shared_ptr<const SplendorPersistentNode<NPlayers>> node_{};
};

template <int NPlayers>
struct SplendorState final : public CloneableState<SplendorState<NPlayers>> {
  using Cfg = SplendorConfig<NPlayers>;
  SplendorPersistentState<NPlayers> persistent{};
  std::uint64_t rng_salt = 0;
  std::vector<SplendorPersistentState<NPlayers>> undo_stack{};

  SplendorState();
  void reset_with_seed(std::uint64_t seed) override;

  StateHash64 state_hash(bool include_hidden_rng) const override;
  void hash_public_fields(Hasher& h) const override;
  void hash_private_fields(int player, Hasher& h) const override;
  int current_player() const override;
  int first_player() const override;
  bool is_terminal() const override;
  bool is_turn_start() const override;
  int num_players() const override { return Cfg::kPlayers; }
  int winner() const override;
};

const std::vector<SplendorCard>& splendor_card_pool();
const std::array<std::array<std::int8_t, kColorCount>, 12>& splendor_nobles();

extern template struct SplendorData<2>;
extern template struct SplendorData<3>;
extern template struct SplendorData<4>;
extern template class SplendorPersistentState<2>;
extern template class SplendorPersistentState<3>;
extern template class SplendorPersistentState<4>;
extern template struct SplendorState<2>;
extern template struct SplendorState<3>;
extern template struct SplendorState<4>;

}  // namespace board_ai::splendor
