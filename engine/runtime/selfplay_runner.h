#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../core/game_interfaces.h"
#include "../search/net_mcts.h"
#include "../search/temperature_schedule.h"
#include "../search/root_noise.h"

namespace board_ai::runtime {

struct SelfplaySample {
  int ply = 0;
  int player = 0;
  ActionId action_id = -1;
  std::vector<ActionId> policy_action_ids{};
  std::vector<int> policy_action_visits{};
  float z = 0.0f;
};

struct SelfplayEpisodeResult {
  int winner = -1;
  bool draw = false;
  int total_plies = 0;
  std::vector<SelfplaySample> samples{};
};

struct SelfplayConfig {
  int simulations = 200;
  float c_puct = 1.4f;
  int max_depth = 128;
  float value_clip = 1.0f;
  double temperature = 1.0;
  search::TemperatureSchedule temperature_schedule{};
  double dirichlet_alpha = 0.0;
  double dirichlet_epsilon = 0.0;
  int dirichlet_on_first_n_plies = 0;
  int max_game_plies = 500;
};

using TraversalLimiterFactory = std::function<
    std::unique_ptr<search::INetMctsTraversalLimiter>()>;

SelfplayEpisodeResult run_selfplay_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const search::IPolicyValueEvaluator& evaluator,
    const SelfplayConfig& config,
    std::uint64_t episode_seed,
    TraversalLimiterFactory limiter_factory = nullptr);

}  // namespace board_ai::runtime
