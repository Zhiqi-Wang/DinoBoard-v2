#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "../core/belief_tracker.h"
#include "../core/game_registry.h"
#include "../search/net_mcts.h"

namespace board_ai::runtime {

struct NoPeekConfig {
  bool stop_on_stochastic_transition = true;
  bool enable_chance_sampling = true;
  int chance_expand_cap = 10;
};

using StochasticTransitionDetector = board_ai::StochasticTransitionDetector;

class NoPeekTraversalLimiter final : public search::INetMctsTraversalLimiter {
 public:
  NoPeekTraversalLimiter(
      NoPeekConfig config,
      StochasticTransitionDetector detector,
      IBeliefTracker& belief_tracker,
      const IGameRules& rules,
      std::uint64_t seed = 0xB19F3A5D7E991233ULL);

  bool should_stop(
      const IGameState& root_state,
      const IGameState& current_state,
      int depth) const override;

  bool requires_parent_for_stop() const override { return true; }

  bool should_stop_with_parent(
      const IGameState& root_state,
      const IGameState& current_state,
      const IGameState* parent_state,
      ActionId parent_action,
      int depth) const override;

  search::TraversalStopResult on_traversal_stop(
      const IGameState& root_state,
      IGameState& current_state,
      const IGameState* parent_state,
      ActionId parent_action,
      int depth,
      const IGameRules& rules,
      const IStateValueModel& value_model,
      const search::IPolicyValueEvaluator& evaluator) const override;

  void on_ply_complete() const override { chance_pool_.clear(); }

 private:
  NoPeekConfig config_;
  StochasticTransitionDetector detector_;
  IBeliefTracker& belief_tracker_;
  const IGameRules& rules_;
  mutable std::unordered_map<std::uint64_t, std::vector<std::unique_ptr<IGameState>>> chance_pool_;
  mutable std::mt19937_64 rng_;
};

}  // namespace board_ai::runtime
