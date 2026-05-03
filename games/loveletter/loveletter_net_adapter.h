#pragma once

#include <random>
#include <vector>

#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "loveletter_state.h"

namespace board_ai::loveletter {

template <int NPlayers>
class LoveLetterBeliefTracker;

template <int NPlayers>
class LoveLetterFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = LoveLetterConfig<NPlayers>;
  explicit LoveLetterFeatureEncoder(const LoveLetterBeliefTracker<NPlayers>* tracker = nullptr)
      : tracker_(tracker) {}
  int action_space() const override { return kActionSpace; }
  int feature_dim() const override { return Cfg::kFeatureDim; }

  bool encode(
      const IGameState& state,
      int perspective_player,
      const std::vector<ActionId>& legal_actions,
      std::vector<float>* features,
      std::vector<float>* legal_mask) const override;

 private:
  const LoveLetterBeliefTracker<NPlayers>* tracker_ = nullptr;
};

template <int NPlayers>
class LoveLetterBeliefTracker final : public IBeliefTracker {
 public:
  using Cfg = LoveLetterConfig<NPlayers>;
  void init(const IGameState& state, int perspective_player) override;
  void observe_action(
      const IGameState& state_before,
      ActionId action,
      const IGameState& state_after) override;
  void randomize_unseen(IGameState& state, std::mt19937& rng) const override;
  std::map<std::string, std::any> serialize() const override;

  std::int8_t known_hand(int player) const {
    if (player < 0 || player >= Cfg::kPlayers) return 0;
    return known_hand_[player];
  }

 private:
  int perspective_player_ = -1;
  std::array<std::int8_t, Cfg::kPlayers> known_hand_{};
};

extern template class LoveLetterFeatureEncoder<2>;
extern template class LoveLetterFeatureEncoder<3>;
extern template class LoveLetterFeatureEncoder<4>;
extern template class LoveLetterBeliefTracker<2>;
extern template class LoveLetterBeliefTracker<3>;
extern template class LoveLetterBeliefTracker<4>;

}  // namespace board_ai::loveletter
