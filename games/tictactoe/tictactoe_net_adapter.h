#pragma once

#include <vector>

#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "tictactoe_state.h"

namespace board_ai::tictactoe {

constexpr int kActionSpace = 9;
constexpr int kFeatureDim = 28;

// TicTacToe is fully observable — no private information per player.
// `encode_private` is a no-op; `private_feature_dim()` returns 0.
class TicTacToeFeatureEncoder final : public IFeatureEncoder {
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

}  // namespace board_ai::tictactoe
