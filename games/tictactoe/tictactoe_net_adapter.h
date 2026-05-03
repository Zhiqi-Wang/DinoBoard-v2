#pragma once

#include <vector>

#include "../../engine/core/feature_encoder.h"
#include "../../engine/core/game_interfaces.h"
#include "tictactoe_state.h"

namespace board_ai::tictactoe {

constexpr int kActionSpace = 9;
constexpr int kFeatureDim = 28;

class TicTacToeFeatureEncoder final : public IFeatureEncoder {
 public:
  int action_space() const override { return kActionSpace; }
  int feature_dim() const override { return kFeatureDim; }

  bool encode(
      const IGameState& state,
      int perspective_player,
      const std::vector<ActionId>& legal_actions,
      std::vector<float>* features,
      std::vector<float>* legal_mask) const override;
};

}  // namespace board_ai::tictactoe
