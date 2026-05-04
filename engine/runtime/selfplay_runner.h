#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../core/belief_tracker.h"
#include "../core/feature_encoder.h"
#include "../core/game_interfaces.h"
#include "../core/game_registry.h"
#include "../search/net_mcts.h"
#include "../search/tail_solver.h"
#include "../search/temperature_schedule.h"
#include "../search/root_noise.h"

namespace board_ai::runtime {

struct SelfplaySample {
  int ply = 0;
  int player = 0;
  ActionId action_id = -1;
  std::vector<ActionId> policy_action_ids{};
  std::vector<int> policy_action_visits{};
  std::vector<float> features{};
  std::vector<float> legal_mask{};
  float z = 0.0f;
  std::vector<float> z_values{};
  bool tail_solved = false;
  float auxiliary_score = 0.0f;
  bool has_auxiliary_score = false;
};

// Per-ply observation trace for API belief-equivalence tests. Only populated
// when run_selfplay_episode is called with trace_perspective >= 0 AND the
// game has registered a public_event_extractor.
struct SelfplayObservationTrace {
  int ply = 0;
  int actor = 0;                           // player whose action this was
  ActionId action = -1;
  std::vector<std::pair<std::string, AnyMap>> pre_events{};
  std::vector<std::pair<std::string, AnyMap>> post_events{};
  std::map<std::string, std::any> belief_snapshot_after{};
};

struct SelfplayEpisodeResult {
  int winner = -1;
  bool draw = false;
  int total_plies = 0;
  std::vector<SelfplaySample> samples{};
  std::map<std::string, double> custom_stats{};
  std::unique_ptr<IGameState> final_state{};

  int tail_solve_attempts = 0;
  int tail_solve_completed = 0;
  int tail_solve_successes = 0;
  double tail_solve_total_ms = 0.0;

  bool trace_enabled = false;
  int trace_perspective = -1;
  std::map<std::string, std::any> initial_belief_snapshot{};
  AnyMap initial_observation{};
  std::vector<SelfplayObservationTrace> observation_trace{};
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

  bool tail_solve_enabled = false;
  int tail_solve_start_ply = 40;
  search::TailSolveConfig tail_solve_config{};

  double heuristic_guidance_ratio = 0.0;
  double heuristic_temperature = 0.0;
  double training_filter_ratio = 1.0;
};

using GameAdjudicator = board_ai::GameAdjudicator;
using AuxiliaryScorer = board_ai::AuxiliaryScorer;
using HeuristicPicker = board_ai::HeuristicPicker;
using TrainingActionFilter = board_ai::TrainingActionFilter;
using TailSolveTrigger = board_ai::TailSolveTrigger;

class FilteredRulesWrapper final : public IGameRules {
 public:
  FilteredRulesWrapper(const IGameRules& inner, TrainingActionFilter filter)
      : inner_(inner), filter_(std::move(filter)) {}

  bool validate_action(const IGameState& state, ActionId action) const override {
    return inner_.validate_action(state, action);
  }

  std::vector<ActionId> legal_actions(const IGameState& state) const override {
    auto legal = inner_.legal_actions(state);
    if (filter_) {
      auto filtered = filter_(const_cast<IGameState&>(state), inner_, legal);
      if (!filtered.empty()) return filtered;
    }
    return legal;
  }

  UndoToken do_action_fast(IGameState& state, ActionId action) const override {
    return inner_.do_action_fast(state, action);
  }

  void undo_action(IGameState& state, const UndoToken& token) const override {
    inner_.undo_action(state, token);
  }

  UndoToken do_action_deterministic(IGameState& state, ActionId action) const override {
    return inner_.do_action_deterministic(state, action);
  }

 private:
  const IGameRules& inner_;
  TrainingActionFilter filter_;
};

using PublicEventExtractor = board_ai::PublicEventExtractor;
using InitialObservationExtractor = board_ai::InitialObservationExtractor;

SelfplayEpisodeResult run_selfplay_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const search::IPolicyValueEvaluator& evaluator,
    const SelfplayConfig& config,
    std::uint64_t episode_seed,
    IBeliefTracker* belief_tracker = nullptr,
    const IFeatureEncoder* encoder = nullptr,
    const search::ITailSolver* tail_solver = nullptr,
    GameAdjudicator adjudicator = nullptr,
    AuxiliaryScorer auxiliary_scorer = nullptr,
    HeuristicPicker heuristic_picker = nullptr,
    TrainingActionFilter training_action_filter = nullptr,
    TailSolveTrigger tail_solve_trigger = nullptr,
    EpisodeStatsExtractor episode_stats_extractor = nullptr,
    // Tracing hooks for AI API belief-equivalence tests. trace_perspective
    // >= 0 enables recording; -1 disables (default, zero overhead).
    // IMPORTANT: trace_belief_tracker must be a SEPARATE tracker instance
    // dedicated to the trace_perspective — the primary belief_tracker is
    // re-init'd to current_player each ply (for MCTS NoPeek), which clobbers
    // any perspective-stable belief. The caller is responsible for creating
    // this second instance via the game's factory.
    int trace_perspective = -1,
    IBeliefTracker* trace_belief_tracker = nullptr,
    PublicEventExtractor public_event_extractor = nullptr,
    InitialObservationExtractor initial_observation_extractor = nullptr);

}  // namespace board_ai::runtime
