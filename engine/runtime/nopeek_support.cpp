#include "nopeek_support.h"

#include <algorithm>

namespace board_ai::runtime {

NoPeekTraversalLimiter::NoPeekTraversalLimiter(
    NoPeekConfig config,
    StochasticTransitionDetector detector,
    IBeliefTracker& belief_tracker,
    const IGameRules& rules,
    std::uint64_t seed)
    : config_(config),
      detector_(std::move(detector)),
      belief_tracker_(belief_tracker),
      rules_(rules),
      rng_(seed) {}

bool NoPeekTraversalLimiter::should_stop(
    const IGameState& /*root_state*/,
    const IGameState& /*current_state*/,
    int /*depth*/) const {
  return false;
}

bool NoPeekTraversalLimiter::should_stop_with_parent(
    const IGameState& /*root_state*/,
    const IGameState& current_state,
    const IGameState* parent_state,
    ActionId /*parent_action*/,
    int /*depth*/) const {
  if (!config_.stop_on_stochastic_transition) return false;
  if (!parent_state) return false;
  return detector_(*parent_state, current_state);
}

search::TraversalStopResult NoPeekTraversalLimiter::on_traversal_stop(
    const IGameState& /*root_state*/,
    IGameState& current_state,
    const IGameState* parent_state,
    ActionId parent_action,
    int /*depth*/,
    const IGameRules& rules,
    const IStateValueModel& /*value_model*/,
    const search::IPolicyValueEvaluator& /*evaluator*/) const {
  search::TraversalStopResult out{};

  if (!config_.enable_chance_sampling || !parent_state || parent_action < 0) {
    return out;
  }

  const int cap = std::max(1, config_.chance_expand_cap);
  std::uint64_t key = static_cast<std::uint64_t>(parent_state->state_hash(false));
  key ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(parent_action))
         + kGoldenRatio64 + (key << 6U) + (key >> 2U);

  auto& pool = chance_pool_[key];

  if (static_cast<int>(pool.size()) < cap) {
    auto sampled = parent_state->clone_state();
    std::mt19937 sample_rng(static_cast<std::uint32_t>(rng_()));
    belief_tracker_.randomize_unseen(*sampled, sample_rng);

    if (rules.validate_action(*sampled, parent_action)) {
      rules.do_action_fast(*sampled, parent_action);
      pool.push_back(std::move(sampled));
    }
  }

  if (pool.empty()) return out;

  const size_t idx = static_cast<size_t>(rng_() % static_cast<std::uint64_t>(pool.size()));
  current_state.copy_from(*pool[idx]);

  out.action = search::TraversalStopAction::kContinue;
  return out;
}

}  // namespace board_ai::runtime
