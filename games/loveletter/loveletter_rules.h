#pragma once

#include <vector>

#include "../../engine/core/game_interfaces.h"
#include "loveletter_state.h"

namespace board_ai::loveletter {

template <int NPlayers>
class LoveLetterRules final : public IGameRules {
 public:
  using Cfg = LoveLetterConfig<NPlayers>;

  bool validate_action(const IGameState& state, ActionId action) const override;
  std::vector<ActionId> legal_actions(const IGameState& state) const override;
  UndoToken do_action_fast(IGameState& state, ActionId action) const override;
  void undo_action(IGameState& state, const UndoToken& token) const override;
};

extern template class LoveLetterRules<2>;
extern template class LoveLetterRules<3>;
extern template class LoveLetterRules<4>;

}  // namespace board_ai::loveletter
