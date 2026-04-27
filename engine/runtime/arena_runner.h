#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "../core/game_interfaces.h"
#include "../search/net_mcts.h"

namespace board_ai::runtime {

struct ArenaPlayerConfig {
  int simulations = 200;
  float c_puct = 1.4f;
  int max_depth = 128;
  float value_clip = 1.0f;
  double temperature = 0.0;
};

struct ArenaMatchResult {
  int winner = -1;
  bool draw = false;
  int total_plies = 0;
};

using PolicyEvaluatorFactory = std::function<
    const search::IPolicyValueEvaluator&(int player_index)>;

ArenaMatchResult run_arena_match(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    PolicyEvaluatorFactory evaluator_for_player,
    const ArenaPlayerConfig& player0_config,
    const ArenaPlayerConfig& player1_config,
    int max_game_plies = 500,
    std::uint64_t match_seed = 0);

}  // namespace board_ai::runtime
