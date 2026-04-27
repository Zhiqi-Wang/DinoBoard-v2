#include "arena_runner.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace board_ai::runtime {

ArenaMatchResult run_arena_match(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    PolicyEvaluatorFactory evaluator_for_player,
    const ArenaPlayerConfig& player0_config,
    const ArenaPlayerConfig& player1_config,
    int max_game_plies,
    std::uint64_t match_seed) {
  ArenaMatchResult result{};
  auto state = initial_state.clone_state();
  int ply = 0;

  std::array<const ArenaPlayerConfig*, 2> configs = {&player0_config, &player1_config};

  while (!value_model.is_terminal(*state) && ply < max_game_plies) {
    const int player = value_model.current_player(*state);
    const auto legal = rules.legal_actions(*state);
    if (legal.empty()) break;

    const int cfg_idx = std::min(player, 1);
    const ArenaPlayerConfig& pcfg = *configs[cfg_idx];
    const search::IPolicyValueEvaluator& eval = evaluator_for_player(player);

    search::NetMctsConfig mcts_cfg{};
    mcts_cfg.simulations = pcfg.simulations;
    mcts_cfg.c_puct = pcfg.c_puct;
    mcts_cfg.max_depth = pcfg.max_depth;
    mcts_cfg.value_clip = pcfg.value_clip;

    search::NetMcts mcts(mcts_cfg);
    search::NetMctsStats stats{};
    mcts.search_root(*state, rules, value_model, eval, &stats);

    const std::uint64_t action_seed = match_seed ^ (static_cast<std::uint64_t>(ply) * 0x9e3779b97f4a7c15ULL);
    const ActionId chosen = search::select_action_from_visits(
        stats.root_actions, stats.root_action_visits, pcfg.temperature, action_seed, legal[0]);

    rules.do_action_fast(*state, chosen);
    ply += 1;
  }

  result.total_plies = ply;

  if (value_model.is_terminal(*state)) {
    const int num_players = state->num_players();
    float best_val = -2.0f;
    int best_player = -1;
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (v > best_val) {
        best_val = v;
        best_player = p;
      }
    }
    bool is_draw = true;
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (std::abs(v - best_val) > 1e-6f) {
        is_draw = false;
        break;
      }
    }
    result.draw = is_draw;
    result.winner = is_draw ? -1 : best_player;
  }

  return result;
}

}  // namespace board_ai::runtime
