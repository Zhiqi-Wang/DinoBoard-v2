#pragma once

#include <vector>

#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "quoridor_state.h"

namespace board_ai::quoridor {

constexpr int kFeatureDim = 295;

// Quoridor is fully observable.
class QuoridorFeatureEncoder final : public IFeatureEncoder {
 public:
  int action_space() const override { return kActionSpace; }
  int feature_dim() const override { return kFeatureDim; }
  int public_feature_dim() const override { return kFeatureDim; }
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

}  // namespace board_ai::quoridor
