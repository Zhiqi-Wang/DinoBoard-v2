#include "tictactoe_net_adapter.h"

namespace board_ai::tictactoe {

void TicTacToeFeatureEncoder::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const TicTacToeState*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= kPlayers) {
    return;
  }
  const int opp = 1 - perspective_player;

  for (int i = 0; i < kBoardSize; ++i) {
    const int v = static_cast<int>(s->board[static_cast<size_t>(i)]);
    out->push_back(v == perspective_player ? 1.0f : 0.0f);
    out->push_back(v == opp ? 1.0f : 0.0f);
    out->push_back(v == kEmptyCell ? 1.0f : 0.0f);
  }
  out->push_back(s->first_player() == perspective_player ? 1.0f : 0.0f);
}

}  // namespace board_ai::tictactoe
