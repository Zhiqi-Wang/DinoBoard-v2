#include "quoridor_net_adapter.h"
#include "quoridor_rules.h"

namespace board_ai::quoridor {

void QuoridorFeatureEncoder::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const QuoridorState*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= kPlayers) {
    return;
  }
  const int opp = 1 - perspective_player;
  const int me_r = s->pawn_row[static_cast<size_t>(perspective_player)];
  const int me_c = s->pawn_col[static_cast<size_t>(perspective_player)];
  const int opp_r = s->pawn_row[static_cast<size_t>(opp)];
  const int opp_c = s->pawn_col[static_cast<size_t>(opp)];

  // My pawn one-hot (81 dims: 9x9 grid)
  for (int r = 0; r < kBoardSize; ++r) {
    for (int c = 0; c < kBoardSize; ++c) {
      out->push_back((r == me_r && c == me_c) ? 1.0f : 0.0f);
    }
  }

  // Opponent pawn one-hot (81 dims)
  for (int r = 0; r < kBoardSize; ++r) {
    for (int c = 0; c < kBoardSize; ++c) {
      out->push_back((r == opp_r && c == opp_c) ? 1.0f : 0.0f);
    }
  }

  // Horizontal walls bitmap (64 dims)
  for (int r = 0; r < kWallGrid; ++r) {
    for (int c = 0; c < kWallGrid; ++c) {
      out->push_back(s->h_walls[static_cast<size_t>(wall_index(r, c))] ? 1.0f : 0.0f);
    }
  }

  // Vertical walls bitmap (64 dims)
  for (int r = 0; r < kWallGrid; ++r) {
    for (int c = 0; c < kWallGrid; ++c) {
      out->push_back(s->v_walls[static_cast<size_t>(wall_index(r, c))] ? 1.0f : 0.0f);
    }
  }

  // Walls remaining (2 dims)
  out->push_back(static_cast<float>(s->walls_remaining[static_cast<size_t>(perspective_player)]) /
                 static_cast<float>(kMaxWallsPerPlayer));
  out->push_back(static_cast<float>(s->walls_remaining[static_cast<size_t>(opp)]) /
                 static_cast<float>(kMaxWallsPerPlayer));

  // Player-0 indicator (1 dim)
  out->push_back(perspective_player == 0 ? 1.0f : 0.0f);

  // Shortest-path distances (2 dims)
  const float my_dist = static_cast<float>(
      QuoridorRules::shortest_path_distance(*s, perspective_player)) / 16.0f;
  const float opp_dist = static_cast<float>(
      QuoridorRules::shortest_path_distance(*s, opp)) / 16.0f;
  out->push_back(my_dist);
  out->push_back(opp_dist);
}

}  // namespace board_ai::quoridor
