#pragma once

#include <random>
#include <unordered_set>
#include <vector>

#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "splendor_state.h"

namespace board_ai::splendor {

template <int NPlayers>
class SplendorFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = SplendorConfig<NPlayers>;
  int action_space() const override { return Cfg::kActionSpace; }
  int feature_dim() const override { return Cfg::kFeatureDim; }

  bool encode(
      const IGameState& state,
      int perspective_player,
      const std::vector<ActionId>& legal_actions,
      std::vector<float>* features,
      std::vector<float>* legal_mask) const override;
};

template <int NPlayers>
class SplendorBeliefTracker final : public IBeliefTracker {
 public:
  using Cfg = SplendorConfig<NPlayers>;
  void init(const IGameState& state, int perspective_player) override;
  void observe_action(
      const IGameState& state_before,
      ActionId action,
      const IGameState& state_after) override;
  void randomize_unseen(IGameState& state, std::mt19937& rng) const override;
  std::map<std::string, std::any> serialize() const override;

 private:
  int perspective_player_ = -1;
  std::unordered_set<int> seen_cards_;
  bool initialized_ = false;
};

extern template class SplendorFeatureEncoder<2>;
extern template class SplendorFeatureEncoder<3>;
extern template class SplendorFeatureEncoder<4>;
extern template class SplendorBeliefTracker<2>;
extern template class SplendorBeliefTracker<3>;
extern template class SplendorBeliefTracker<4>;

}  // namespace board_ai::splendor
