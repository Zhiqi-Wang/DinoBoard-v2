#include "tictactoe_net_adapter.h"

namespace board_ai::tictactoe {

bool TicTacToeFeatureEncoder::encode(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* features,
    std::vector<float>* legal_mask) const {
  const auto* s = dynamic_cast<const TicTacToeState*>(&state);
  if (!s || !features || !legal_mask || perspective_player < 0 || perspective_player >= kPlayers) {
    return false;
  }
  const int opp = 1 - perspective_player;

  features->clear();
  features->reserve(static_cast<size_t>(feature_dim()));
  for (int i = 0; i < kBoardSize; ++i) {
    const int v = static_cast<int>(s->board[static_cast<size_t>(i)]);
    features->push_back(v == perspective_player ? 1.0f : 0.0f);
    features->push_back(v == opp ? 1.0f : 0.0f);
    features->push_back(v == kEmptyCell ? 1.0f : 0.0f);
  }
  features->push_back(s->first_player() == perspective_player ? 1.0f : 0.0f);

  fill_legal_mask(action_space(), legal_actions, legal_mask);

  return static_cast<int>(features->size()) == feature_dim();
}

}  // namespace board_ai::tictactoe
