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
    const std::vector<ArenaPlayerConfig>& player_configs,
    int max_game_plies,
    std::uint64_t match_seed,
    TraversalLimiterFactory limiter_factory,
    IBeliefTracker* belief_tracker,
    GameAdjudicator adjudicator) {
  ArenaMatchResult result{};
  auto state = initial_state.clone_state();
  int ply = 0;

  std::unique_ptr<search::INetMctsTraversalLimiter> limiter;
  if (limiter_factory) {
    limiter = limiter_factory();
  }

  while (!state->is_terminal() && ply < max_game_plies) {
    const int player = state->current_player();
    const auto legal = rules.legal_actions(*state);
    if (legal.empty()) break;

    const size_t cfg_idx = player_configs.empty()
        ? 0 : static_cast<size_t>(player) % player_configs.size();
    const ArenaPlayerConfig& pcfg = player_configs.empty()
        ? ArenaPlayerConfig{} : player_configs[cfg_idx];
    const search::IPolicyValueEvaluator& eval = evaluator_for_player(player);

    if (belief_tracker) {
      belief_tracker->init(*state, player);
    }

    search::NetMctsConfig mcts_cfg{};
    mcts_cfg.simulations = pcfg.simulations;
    mcts_cfg.c_puct = pcfg.c_puct;
    mcts_cfg.max_depth = pcfg.max_depth;
    mcts_cfg.value_clip = pcfg.value_clip;
    mcts_cfg.traversal_limiter = limiter.get();

    if (pcfg.tail_solve_enabled && pcfg.tail_solver) {
      bool try_ts = false;
      if (pcfg.tail_solve_trigger) {
        try_ts = pcfg.tail_solve_trigger(*state, ply);
      } else {
        try_ts = true;
      }
      if (try_ts) {
        mcts_cfg.tail_solve_enabled = true;
        mcts_cfg.tail_solve_config = pcfg.tail_solve_config;
        mcts_cfg.tail_solver = pcfg.tail_solver;
      }
    }

    search::NetMcts mcts(mcts_cfg);
    search::NetMctsStats stats{};
    const std::uint64_t mcts_seed = match_seed ^
        (static_cast<std::uint64_t>(ply) * kGoldenRatio64) ^ 0x243F6A8885A308D3ULL;
    mcts.search_root(*state, rules, value_model, eval, &stats, mcts_seed);

    result.total_traversal_stops += stats.traversal_stops;
    const std::uint64_t action_seed = match_seed ^ (static_cast<std::uint64_t>(ply) * kGoldenRatio64);
    const ActionId chosen = search::select_action_from_visits(
        stats.root_actions, stats.root_action_visits, pcfg.temperature, action_seed, legal[0]);

    result.action_history.push_back(chosen);
    result.ply_stats.push_back({stats.tail_solved, stats.tail_solve_value});
    std::unique_ptr<IGameState> state_before;
    if (belief_tracker) state_before = state->clone_state();
    rules.do_action_fast(*state, chosen);
    if (belief_tracker) belief_tracker->observe_action(*state_before, chosen, *state);
    if (limiter) limiter->on_ply_complete();
    ply += 1;
  }

  result.total_plies = ply;

  if (!state->is_terminal() && adjudicator) {
    const int adj_winner = adjudicator(*state);
    if (adj_winner < 0) {
      result.draw = true;
      result.winner = -1;
    } else {
      result.draw = false;
      result.winner = adj_winner;
    }
    return result;
  }

  if (state->is_terminal()) {
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
  } else {
    result.draw = true;
    result.winner = -1;
  }

  return result;
}

}  // namespace board_ai::runtime
