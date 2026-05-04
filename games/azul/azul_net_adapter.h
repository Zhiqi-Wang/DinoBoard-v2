#pragma once

#include <random>
#include <vector>

#include "azul_state.h"
#include "../../engine/core/belief_tracker.h"
#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"

namespace board_ai::azul {

// Azul has symmetric random state (bag contents) but no per-player hidden
// fields — every player sees the same public info. private_feature_dim()
// is 0.
template <int NPlayers>
class AzulFeatureEncoder final : public IFeatureEncoder {
 public:
  using Cfg = AzulConfig<NPlayers>;
  int action_space() const override { return Cfg::kActionSpace; }
  int feature_dim() const override { return Cfg::kFeatureDim; }
  int public_feature_dim() const override { return Cfg::kFeatureDim; }
  int private_feature_dim() const override { return 0; }

  void encode_public(
      const IGameState& state,
      int perspective_player,
      std::vector<float>* out) const override;

  void encode_private(
      const IGameState& /*state*/,
      int /*player*/,
      std::vector<float>* /*out*/) const override {}
};

template <int NPlayers>
class AzulBeliefTracker final : public IBeliefTracker {
 public:
  void init(int perspective_player, const AnyMap& initial_observation) override;
  void observe_public_event(
      int actor,
      ActionId action,
      const std::vector<PublicEvent>& pre_events,
      const std::vector<PublicEvent>& post_events) override;
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
