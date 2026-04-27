#include "selfplay_runner.h"

#include <algorithm>
#include <cmath>

namespace board_ai::runtime {

SelfplayEpisodeResult run_selfplay_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const search::IPolicyValueEvaluator& evaluator,
    const SelfplayConfig& config,
    std::uint64_t episode_seed,
    TraversalLimiterFactory limiter_factory) {
  SelfplayEpisodeResult result{};

  std::unique_ptr<search::INetMctsTraversalLimiter> limiter;
  if (limiter_factory) {
    limiter = limiter_factory();
  }

  auto state = initial_state.clone_state();
  int ply = 0;

  while (!value_model.is_terminal(*state) && ply < config.max_game_plies) {
    const int player = value_model.current_player(*state);
    const auto legal = rules.legal_actions(*state);
    if (legal.empty()) break;

    const auto noise = search::resolve_root_dirichlet_noise(
        config.dirichlet_alpha, config.dirichlet_epsilon,
        config.dirichlet_on_first_n_plies, ply);

    search::NetMctsConfig mcts_cfg{};
    mcts_cfg.simulations = config.simulations;
    mcts_cfg.c_puct = config.c_puct;
    mcts_cfg.max_depth = config.max_depth;
    mcts_cfg.value_clip = config.value_clip;
    mcts_cfg.root_dirichlet_alpha = noise.alpha;
    mcts_cfg.root_dirichlet_epsilon = noise.epsilon;
    mcts_cfg.traversal_limiter = limiter.get();

    search::NetMcts mcts(mcts_cfg);
    search::NetMctsStats stats{};
    mcts.search_root(*state, rules, value_model, evaluator, &stats);

    SelfplaySample sample{};
    sample.ply = ply;
    sample.player = player;
    sample.policy_action_ids = stats.root_actions;
    sample.policy_action_visits = stats.root_action_visits;

    const double temperature = search::resolve_linear_temperature(
        config.temperature_schedule, config.temperature, ply);
    const std::uint64_t action_seed = episode_seed ^ (static_cast<std::uint64_t>(ply) * 0x9e3779b97f4a7c15ULL);
    const ActionId chosen = search::select_action_from_visits(
        stats.root_actions, stats.root_action_visits, temperature, action_seed, legal[0]);

    sample.action_id = chosen;
    result.samples.push_back(std::move(sample));

    rules.do_action_fast(*state, chosen);
    ply += 1;
  }

  result.total_plies = ply;

  if (value_model.is_terminal(*state)) {
    const int num_players = state->num_players();
    float best_val = -2.0f;
    int best_player = -1;
    bool is_draw = true;
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (v > best_val) {
        best_val = v;
        best_player = p;
      }
    }
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (std::abs(v - best_val) > 1e-6f) {
        is_draw = false;
        break;
      }
    }
    if (is_draw) {
      result.draw = true;
      result.winner = -1;
    } else {
      result.draw = false;
      result.winner = best_player;
    }

    for (auto& s : result.samples) {
      if (result.draw) {
        s.z = 0.0f;
      } else {
        s.z = (s.player == result.winner) ? 1.0f : -1.0f;
      }
    }
  }

  return result;
}

}  // namespace board_ai::runtime
