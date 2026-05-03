#include <array>
#include <cmath>
#include <vector>

#include "../../engine/core/game_registry.h"
#include "../../engine/search/tail_solver.h"
#include "quoridor_state.h"
#include "quoridor_rules.h"
#include "quoridor_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::ActionId;
using board_ai::HeuristicResult;
using board_ai::IGameState;
using board_ai::IGameRules;
using namespace board_ai::quoridor;

AnyMap serialize_quoridor(const IGameState& state) {
  const auto& s = board_ai::checked_cast<QuoridorState>(state);
  AnyMap m;
  m["current_player"] = std::any(s.current_player());
  m["is_terminal"] = std::any(s.is_terminal());
  m["winner"] = std::any(s.winner());
  m["move_count"] = std::any(s.move_count);
  m["board_size"] = std::any(static_cast<int>(kBoardSize));

  std::vector<int> scores(kPlayers);
  for (int i = 0; i < kPlayers; ++i) scores[static_cast<size_t>(i)] = s.scores[static_cast<size_t>(i)];
  m["scores"] = std::any(scores);

  std::vector<AnyMap> pawns;
  for (int i = 0; i < kPlayers; ++i) {
    pawns.push_back({
        {"player", std::any(i)},
        {"row", std::any(static_cast<int>(s.pawn_row[static_cast<size_t>(i)]))},
        {"col", std::any(static_cast<int>(s.pawn_col[static_cast<size_t>(i)]))},
    });
  }
  m["pawns"] = std::any(pawns);

  std::vector<int> walls_remaining(kPlayers);
  for (int i = 0; i < kPlayers; ++i)
    walls_remaining[static_cast<size_t>(i)] = static_cast<int>(s.walls_remaining[static_cast<size_t>(i)]);
  m["walls_remaining"] = std::any(walls_remaining);

  std::vector<AnyMap> h_walls;
  for (int r = 0; r < kWallGrid; ++r)
    for (int c = 0; c < kWallGrid; ++c)
      if (s.h_walls[static_cast<size_t>(wall_index(r, c))])
        h_walls.push_back({{"row", std::any(r)}, {"col", std::any(c)}});
  m["horizontal_walls"] = std::any(h_walls);

  std::vector<AnyMap> v_walls;
  for (int r = 0; r < kWallGrid; ++r)
    for (int c = 0; c < kWallGrid; ++c)
      if (s.v_walls[static_cast<size_t>(wall_index(r, c))])
        v_walls.push_back({{"row", std::any(r)}, {"col", std::any(c)}});
  m["vertical_walls"] = std::any(v_walls);

  std::vector<int> goal_rows = {goal_row_for_player(0), goal_row_for_player(1)};
  m["goal_rows"] = std::any(goal_rows);
  return m;
}

AnyMap describe_quoridor(ActionId action) {
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));
  if (is_move_action(action)) {
    m["type"] = std::any(std::string("move"));
    m["row"] = std::any(decode_move_row(action));
    m["col"] = std::any(decode_move_col(action));
  } else if (is_hwall_action(action)) {
    m["type"] = std::any(std::string("hwall"));
    m["row"] = std::any(decode_hwall_row(action));
    m["col"] = std::any(decode_hwall_col(action));
  } else if (is_vwall_action(action)) {
    m["type"] = std::any(std::string("vwall"));
    m["row"] = std::any(decode_vwall_row(action));
    m["col"] = std::any(decode_vwall_col(action));
  }
  return m;
}

HeuristicResult heuristic_pick_quoridor(
    IGameState& state, const IGameRules& rules, std::uint64_t /*rng_seed*/) {
  auto legal = rules.legal_actions(state);
  const int me = board_ai::checked_cast<QuoridorState>(state).current_player();
  const int opp = 1 - me;

  HeuristicResult result;
  result.actions = legal;
  result.scores.resize(legal.size());

  for (size_t i = 0; i < legal.size(); ++i) {
    auto clone = state.clone_state();
    rules.do_action_fast(*clone, legal[i]);
    const auto& after = board_ai::checked_cast<QuoridorState>(*clone);
    const int d_me = QuoridorRules::shortest_path_distance(after, me);
    const int d_opp = QuoridorRules::shortest_path_distance(after, opp);
    double score = static_cast<double>(d_opp - d_me);
    result.scores[i] = score;
  }

  return result;
}

board_ai::GameRegistrar reg("quoridor", [](std::uint64_t seed) {
  board_ai::GameBundle b;
  b.game_id = "quoridor";
  auto s = std::make_unique<QuoridorState>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<QuoridorRules>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<QuoridorFeatureEncoder>();
  b.state_serializer = serialize_quoridor;
  b.action_descriptor = describe_quoridor;
  b.heuristic_picker = heuristic_pick_quoridor;
  b.tail_solver = std::make_unique<board_ai::search::AlphaBetaTailSolver>();
  b.tail_solve_trigger = [](const board_ai::IGameState& state, int ply) -> bool {
    if (ply < 20) return false;
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    const int d0 = QuoridorRules::shortest_path_distance(qs, 0);
    const int d1 = QuoridorRules::shortest_path_distance(qs, 1);
    return d0 <= 4 || d1 <= 4;
  };
  b.episode_stats_extractor = [](const board_ai::IGameState&,
      const std::vector<board_ai::SelfplaySampleView>& samples) -> std::map<std::string, double> {
    return {{"turns", static_cast<double>(samples.size()) / 2.0}};
  };
  b.auxiliary_scorer = [](const board_ai::IGameState& state, int player) -> float {
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    int me = QuoridorRules::shortest_path_distance(qs, player);
    int opp = QuoridorRules::shortest_path_distance(qs, 1 - player);
    return std::tanh(static_cast<float>(opp - me) / 8.0f);
  };
  b.adjudicator = [](const board_ai::IGameState& state) -> int {
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    const int d0 = QuoridorRules::shortest_path_distance(qs, 0);
    const int d1 = QuoridorRules::shortest_path_distance(qs, 1);
    if (d0 < d1) return 0;
    if (d1 < d0) return 1;
    return -1;
  };
  b.training_action_filter = [](board_ai::IGameState& state,
      const board_ai::IGameRules& rules,
      const std::vector<board_ai::ActionId>& legal) -> std::vector<board_ai::ActionId> {
    const auto& qs = board_ai::checked_cast<QuoridorState>(state);
    const int me = qs.current_player();
    const int opp = 1 - me;

    const int before_me = QuoridorRules::shortest_path_distance(qs, me);
    const int before_opp = QuoridorRules::shortest_path_distance(qs, opp);
    const int before_gap = before_opp - before_me;

    std::vector<board_ai::ActionId> filtered;
    for (const auto action : legal) {
      if (is_move_action(action)) {
        const int new_pos = decode_move_row(action) * kBoardSize + decode_move_col(action);
        std::array<int, kCellCount> dist_me{};
        QuoridorRules::compute_distance_map(qs, me, &dist_me);
        const int after_me = dist_me[static_cast<size_t>(new_pos)] >= 0
                           ? dist_me[static_cast<size_t>(new_pos)] : 9999;
        const int after_gap = before_opp - after_me;
        if (after_gap > before_gap) filtered.push_back(action);
      } else {
        auto clone = state.clone_state();
        rules.do_action_fast(*clone, action);
        const auto& after_state = board_ai::checked_cast<QuoridorState>(*clone);
        const int after_me = QuoridorRules::shortest_path_distance(after_state, me);
        const int after_opp = QuoridorRules::shortest_path_distance(after_state, opp);
        const int after_gap = after_opp - after_me;
        if (after_gap > before_gap) filtered.push_back(action);
      }
    }
    if (filtered.empty()) return legal;
    return filtered;
  };
  return b;
});
}  // namespace
