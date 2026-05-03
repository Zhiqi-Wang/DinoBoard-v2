#pragma once

#include <cstdint>
#include <cstddef>

#include "../core/game_registry.h"
#include "selfplay_runner.h"

namespace board_ai::runtime {

// Select an index from heuristic scores using softmax-with-temperature sampling.
// temperature <= 1e-6 means greedy argmax (first max on ties); >0 means
// exp(score/T) softmax then cumulative sample with rng_u01 in [0,1).
// Shared by selfplay (run_heuristic_episode) and eval benchmarks so both
// faces of the heuristic use the same selection rule.
std::size_t sample_heuristic_index(
    const std::vector<double>& scores,
    double temperature,
    double rng_u01);

SelfplayEpisodeResult run_heuristic_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const IFeatureEncoder* encoder,
    const HeuristicPicker& heuristic,
    double temperature,
    int max_game_plies,
    std::uint64_t episode_seed,
    AuxiliaryScorer auxiliary_scorer = nullptr,
    GameAdjudicator adjudicator = nullptr);

}  // namespace board_ai::runtime
