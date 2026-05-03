#pragma once

#include <array>
#include <vector>

#include "../../engine/core/game_interfaces.h"
#include "quoridor_state.h"

namespace board_ai::quoridor {

class QuoridorRules final : public IGameRules {
 public:
  bool validate_action(const IGameState& state, ActionId action) const override;
  std::vector<ActionId> legal_actions(const IGameState& state) const override;
  UndoToken do_action_fast(IGameState& state, ActionId action) const override;
  void undo_action(IGameState& state, const UndoToken& token) const override;

  static bool has_path_to_goal(const QuoridorState& state, int player);
  static int shortest_path_distance(const QuoridorState& state, int player);
  static void compute_distance_map(const QuoridorState& state, int player,
                                   std::array<int, kCellCount>* out);

};

}  // namespace board_ai::quoridor
