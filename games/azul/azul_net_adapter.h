#pragma once

#include <random>
#include <vector>

#include "azul_state.h"
#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"

namespace board_ai::azul {

template <int NPlayers>
class AzulFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = AzulConfig<NPlayers>;
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
class AzulBeliefTracker final : public IBeliefTracker {
 public:
  void init(const IGameState& state, int perspective_player) override;
  void observe_action(
      const IGameState& state_before,
      ActionId action,
      const IGameState& state_after) override;
  void randomize_unseen(IGameState& state, std::mt19937& rng) const override;

 private:
  int perspective_player_ = -1;
};

extern template class AzulFeatureEncoder<2>;
extern template class AzulFeatureEncoder<3>;
extern template class AzulFeatureEncoder<4>;
extern template class AzulBeliefTracker<2>;
extern template class AzulBeliefTracker<3>;
extern template class AzulBeliefTracker<4>;

}  // namespace board_ai::azul
