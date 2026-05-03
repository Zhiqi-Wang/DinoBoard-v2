#pragma once

#include <vector>

#include "../../engine/core/game_interfaces.h"
#include "azul_state.h"

namespace board_ai::azul {

template <int NPlayers>
class AzulRules final : public IGameRules {
 public:
  using Cfg = AzulConfig<NPlayers>;

  AzulRules() = default;

  bool validate_action(const IGameState& state, ActionId action) const override;
  std::vector<ActionId> legal_actions(const IGameState& state) const override;
  UndoToken do_action_fast(IGameState& state, ActionId action) const override;
  UndoToken do_action_deterministic(IGameState& state, ActionId action) const override;
  void undo_action(IGameState& state, const UndoToken& token) const override;

 private:
  static int decode_source(ActionId action);
  static int decode_color(ActionId action);
  static int decode_target_line(ActionId action);
  static int wall_col_for_color(int row, int color_idx);
  static int score_wall_placement(const PlayerState& p, int row, int col);
  static int apply_final_bonus(AzulState<NPlayers>& state, int pid);
  static bool will_round_end_after_action(const AzulState<NPlayers>& state, ActionId action);
  static void apply_action_no_undo(AzulState<NPlayers>& state, ActionId action);
  static void apply_round_settlement(AzulState<NPlayers>& state);
};

extern template class AzulRules<2>;
extern template class AzulRules<3>;
extern template class AzulRules<4>;

}  // namespace board_ai::azul
