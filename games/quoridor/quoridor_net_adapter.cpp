#include "quoridor_net_adapter.h"
#include "quoridor_rules.h"

namespace board_ai::quoridor {

bool QuoridorFeatureEncoder::encode(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* features,
    std::vector<float>* legal_mask) const {
  const auto* s = dynamic_cast<const QuoridorState*>(&state);
  if (!s || !features || !legal_mask || perspective_player < 0 || perspective_player >= kPlayers) {
    return false;
  }
  const int opp = 1 - perspective_player;
  const int me_r = s->pawn_row[static_cast<size_t>(perspective_player)];
  const int me_c = s->pawn_col[static_cast<size_t>(perspective_player)];
  const int opp_r = s->pawn_row[static_cast<size_t>(opp)];
  const int opp_c = s->pawn_col[static_cast<size_t>(opp)];

  features->clear();
  features->reserve(static_cast<size_t>(feature_dim()));

  // My pawn one-hot (81 dims: 9x9 grid)
  for (int r = 0; r < kBoardSize; ++r) {
    for (int c = 0; c < kBoardSize; ++c) {
      features->push_back((r == me_r && c == me_c) ? 1.0f : 0.0f);
    }
  }

  // Opponent pawn one-hot (81 dims)
  for (int r = 0; r < kBoardSize; ++r) {
    for (int c = 0; c < kBoardSize; ++c) {
      features->push_back((r == opp_r && c == opp_c) ? 1.0f : 0.0f);
    }
  }

  // Horizontal walls bitmap (64 dims)
  for (int r = 0; r < kWallGrid; ++r) {
    for (int c = 0; c < kWallGrid; ++c) {
      features->push_back(s->h_walls[static_cast<size_t>(wall_index(r, c))] ? 1.0f : 0.0f);
    }
  }

  // Vertical walls bitmap (64 dims)
  for (int r = 0; r < kWallGrid; ++r) {
    for (int c = 0; c < kWallGrid; ++c) {
      features->push_back(s->v_walls[static_cast<size_t>(wall_index(r, c))] ? 1.0f : 0.0f);
    }
  }

  // Walls remaining (2 dims)
  features->push_back(static_cast<float>(s->walls_remaining[static_cast<size_t>(perspective_player)]) /
                      static_cast<float>(kMaxWallsPerPlayer));
  features->push_back(static_cast<float>(s->walls_remaining[static_cast<size_t>(opp)]) /
                      static_cast<float>(kMaxWallsPerPlayer));

  // Player-0 indicator (1 dim)
  features->push_back(perspective_player == 0 ? 1.0f : 0.0f);

  // Shortest-path distances (2 dims)
  const float my_dist = static_cast<float>(
      QuoridorRules::shortest_path_distance(*s, perspective_player)) / 16.0f;
  const float opp_dist = static_cast<float>(
      QuoridorRules::shortest_path_distance(*s, opp)) / 16.0f;
  features->push_back(my_dist);
  features->push_back(opp_dist);

  legal_mask->assign(static_cast<size_t>(action_space()), 0.0f);
  for (ActionId a : legal_actions) {
    if (a >= 0 && a < action_space()) {
      (*legal_mask)[static_cast<size_t>(a)] = 1.0f;
    }
  }

  return static_cast<int>(features->size()) == feature_dim();
}

}  // namespace board_ai::quoridor
