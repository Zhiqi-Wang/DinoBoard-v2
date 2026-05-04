#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../../engine/core/game_interfaces.h"

namespace board_ai::coup {

using CharId = std::int8_t;
constexpr CharId kDuke = 0;
constexpr CharId kAssassin = 1;
constexpr CharId kCaptain = 2;
constexpr CharId kAmbassador = 3;
constexpr CharId kContessa = 4;
constexpr int kCharacterCount = 5;
constexpr int kCardsPerCharacter = 3;
constexpr int kTotalCards = 15;
constexpr int kStartingCoins = 2;
constexpr int kCoupCost = 7;
constexpr int kAssassinateCost = 3;
constexpr int kForceCoupThreshold = 10;

constexpr int kIncomeAction = 0;
constexpr int kForeignAidAction = 1;
constexpr int kCoupOffset = 2;
constexpr int kCoupCount = 4;
constexpr int kTaxAction = 6;
constexpr int kAssassinateOffset = 7;
constexpr int kAssassinateCount = 4;
constexpr int kStealOffset = 11;
constexpr int kStealCount = 4;
constexpr int kExchangeAction = 15;

constexpr int kChallengeAction = 16;
constexpr int kAllowAction = 17;

constexpr int kBlockDukeAction = 18;
constexpr int kBlockContessaAction = 19;
constexpr int kBlockAmbassadorAction = 20;
constexpr int kBlockCaptainAction = 21;
constexpr int kAllowNoBlockAction = 22;

constexpr int kRevealSlot0 = 23;
constexpr int kRevealSlot1 = 24;

constexpr int kLoseSlot0 = 25;
constexpr int kLoseSlot1 = 26;

constexpr int kReturnDuke = 27;
constexpr int kReturnAssassin = 28;
constexpr int kReturnCaptain = 29;
constexpr int kReturnAmbassador = 30;
constexpr int kReturnContessa = 31;

constexpr int kActionSpace = 32;

enum class CoupStage : std::int8_t {
  kDeclareAction = 0,
  kChallengeAction = 1,
  kResolveChallengeAction = 2,
  kChooseLoseInfluence = 3,
  kCounterAction = 4,
  kChallengeCounter = 5,
  kResolveChallengeCounter = 6,
  kChooseLoseInfluenceCounter = 7,
  kExchangeReturn1 = 8,
  kExchangeReturn2 = 9,
  kLoseInfluenceFromAction = 10,
};

template <int NPlayers>
struct CoupConfig {
  static_assert(NPlayers >= 2 && NPlayers <= 4);
  static constexpr int kPlayers = NPlayers;
  // Public per-player (13): alive, coins, inf>=1, inf>=2,
  // revealed-character counts (5), active/target/blocker/challenger (4).
  static constexpr int kPerPlayerPublicFeatures = 13;
  // Private per-player (5): unrevealed-character counts of the player's
  // own hand. Only the owner knows their unrevealed cards.
  static constexpr int kPerPlayerPrivateFeatures = 5;
  static constexpr int kPerPlayerFeatures =
      kPerPlayerPublicFeatures + kPerPlayerPrivateFeatures;  // 18
  static constexpr int kGlobalFeatures = 21;
  static constexpr int kFeatureDim = kPerPlayerFeatures * NPlayers + kGlobalFeatures;
  static constexpr int kPublicFeatureDim =
      kPerPlayerPublicFeatures * NPlayers + kGlobalFeatures;
  static constexpr int kPrivateFeatureDim =
      kPerPlayerPrivateFeatures * NPlayers;
};

template <int NPlayers>
struct CoupData {
  using Cfg = CoupConfig<NPlayers>;

  int current_player = 0;
  std::int8_t first_player = 0;
  int winner = -1;
  bool terminal = false;
  int ply = 0;
  CoupStage stage = CoupStage::kDeclareAction;

  std::array<std::array<CharId, 2>, Cfg::kPlayers> influence{};
  std::array<std::array<bool, 2>, Cfg::kPlayers> revealed{};
  std::array<int, Cfg::kPlayers> coins{};
  std::array<bool, Cfg::kPlayers> alive{};

  std::vector<CharId> court_deck;

  int active_player = 0;
  ActionId declared_action = -1;
  int action_target = -1;
  CharId claimed_character = -1;

  int challenger = -1;
  int challenge_loser = -1;
  bool action_challenged = false;
  bool action_challenge_succeeded = false;

  int blocker = -1;
  CharId block_character = -1;
  bool counter_challenged = false;
  bool counter_challenge_succeeded = false;

  int challenge_check_index = 0;

  std::array<CharId, 2> exchange_drawn{};
  int exchange_held_count = 0;

  std::uint64_t draw_nonce = 0;
};

template <int NPlayers>
struct CoupState final : public CloneableState<CoupState<NPlayers>> {
  using Cfg = CoupConfig<NPlayers>;
  CoupData<NPlayers> data;
  std::uint64_t rng_salt = 0;
  std::vector<CoupData<NPlayers>> undo_stack;

  CoupState();
  void reset_with_seed(std::uint64_t seed) override;
  StateHash64 state_hash(bool include_hidden_rng) const override;
  void hash_public_fields(Hasher& h) const override;
  void hash_private_fields(int player, Hasher& h) const override;
  int current_player() const override;
  int first_player() const override;
  bool is_terminal() const override;
  int num_players() const override { return Cfg::kPlayers; }
  int winner() const override;
  bool is_turn_start() const override;
};

extern template struct CoupData<2>;
extern template struct CoupData<3>;
extern template struct CoupData<4>;
extern template struct CoupState<2>;
extern template struct CoupState<3>;
extern template struct CoupState<4>;

}  // namespace board_ai::coup
