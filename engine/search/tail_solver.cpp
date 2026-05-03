#include "tail_solver.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace board_ai::search {

namespace {

enum class TTFlag : std::int8_t { kExact, kLowerBound, kUpperBound };

struct TTEntry {
  float value = 0.0f;
  int depth = 0;
  TTFlag flag = TTFlag::kExact;
  ActionId best_action = -1;
};

struct SearchContext {
  const IGameRules& rules;
  const IStateValueModel& value_model;
  int perspective;
  int num_players;
  std::int64_t node_budget;
  std::int64_t nodes_searched = 0;
  bool budget_exceeded = false;
  std::chrono::steady_clock::time_point deadline{};
  bool has_deadline = false;
  float margin_weight = 0.0f;
  MarginScorer margin_scorer;
  std::unordered_map<StateHash64, TTEntry> tt{};
};

float alpha_beta(
    IGameState& state,
    SearchContext& ctx,
    int depth_remaining,
    float alpha,
    float beta) {
  if (ctx.budget_exceeded) return 0.0f;
  ctx.nodes_searched += 1;

  if (ctx.nodes_searched >= ctx.node_budget) {
    ctx.budget_exceeded = true;
    return 0.0f;
  }

  if (ctx.has_deadline && (ctx.nodes_searched & 1023) == 0) {
    if (std::chrono::steady_clock::now() >= ctx.deadline) {
      ctx.budget_exceeded = true;
      return 0.0f;
    }
  }

  if (state.is_terminal()) {
    float v = ctx.value_model.terminal_value_for_player(state, ctx.perspective);
    if (ctx.margin_weight != 0.0f && ctx.margin_scorer) {
      v += ctx.margin_weight * ctx.margin_scorer(state, ctx.perspective);
    }
    return v;
  }

  if (depth_remaining <= 0) {
    return 0.0f;
  }

  // Mix current_player into TT key: a paranoid-search node's value depends on
  // whether we are maximizing (perspective player to move) or minimizing (any
  // other player to move). If a game's state_hash(false) happens to collide
  // across turns with different players-to-move, TT entries from one role
  // would poison the other. We do not rely on game authors to encode
  // current_player into state_hash — this mixes it in defensively.
  const StateHash64 hash = state.state_hash(false) ^
      (static_cast<StateHash64>(state.current_player()) * kGoldenRatio64);
  auto tt_it = ctx.tt.find(hash);
  if (tt_it != ctx.tt.end()) {
    const TTEntry& entry = tt_it->second;
    if (entry.depth >= depth_remaining) {
      if (entry.flag == TTFlag::kExact) return entry.value;
      if (entry.flag == TTFlag::kLowerBound && entry.value >= beta) return entry.value;
      if (entry.flag == TTFlag::kUpperBound && entry.value <= alpha) return entry.value;
    }
  }

  const auto legal = ctx.rules.legal_actions(state);
  if (legal.empty()) {
    return ctx.value_model.terminal_value_for_player(state, ctx.perspective);
  }

  const bool is_maximizing = (state.current_player() == ctx.perspective);

  ActionId best_action = legal[0];

  // Move ordering: try TT best move first
  std::vector<ActionId> ordered = legal;
  if (tt_it != ctx.tt.end() && tt_it->second.best_action >= 0) {
    ActionId tt_best = tt_it->second.best_action;
    for (size_t i = 1; i < ordered.size(); ++i) {
      if (ordered[i] == tt_best) {
        std::swap(ordered[0], ordered[i]);
        break;
      }
    }
  }

  float best_value;
  TTFlag flag;

  if (is_maximizing) {
    best_value = -2.0f;
    flag = TTFlag::kUpperBound;
    for (const ActionId action : ordered) {
      const UndoToken tok = ctx.rules.do_action_deterministic(state, action);
      const float val = alpha_beta(state, ctx, depth_remaining - 1, alpha, beta);
      ctx.rules.undo_action(state, tok);

      if (ctx.budget_exceeded) return 0.0f;

      if (val > best_value) {
        best_value = val;
        best_action = action;
      }
      if (best_value > alpha) {
        alpha = best_value;
        flag = TTFlag::kExact;
      }
      if (alpha >= beta) {
        flag = TTFlag::kLowerBound;
        break;
      }
    }
  } else {
    best_value = 2.0f;
    flag = TTFlag::kLowerBound;
    for (const ActionId action : ordered) {
      const UndoToken tok = ctx.rules.do_action_deterministic(state, action);
      const float val = alpha_beta(state, ctx, depth_remaining - 1, alpha, beta);
      ctx.rules.undo_action(state, tok);

      if (ctx.budget_exceeded) return 0.0f;

      if (val < best_value) {
        best_value = val;
        best_action = action;
      }
      if (best_value < beta) {
        beta = best_value;
        flag = TTFlag::kExact;
      }
      if (alpha >= beta) {
        flag = TTFlag::kUpperBound;
        break;
      }
    }
  }

  TTEntry new_entry{};
  new_entry.value = best_value;
  new_entry.depth = depth_remaining;
  new_entry.flag = flag;
  new_entry.best_action = best_action;
  ctx.tt[hash] = new_entry;

  return best_value;
}

}  // namespace

TailSolveResult AlphaBetaTailSolver::solve(
    IGameState& state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    int perspective_player,
    const TailSolveConfig& config) const {
  const auto t0 = std::chrono::steady_clock::now();

  SearchContext ctx{rules, value_model, perspective_player,
                    state.num_players(), config.node_budget};
  ctx.margin_weight = config.margin_weight;
  ctx.margin_scorer = config.margin_scorer;

  if (config.time_limit_ms > 0) {
    ctx.has_deadline = true;
    ctx.deadline = t0 + std::chrono::milliseconds(config.time_limit_ms);
  }

  ctx.tt.reserve(std::min(config.node_budget, std::int64_t(1'000'000)));

  float result_value = 0.0f;
  ActionId best_action = -1;

  // Iterative deepening up to depth_limit
  for (int depth = 1; depth <= config.depth_limit; ++depth) {
    ctx.budget_exceeded = false;
    const float val = alpha_beta(state, ctx, depth, -2.0f, 2.0f);

    if (ctx.budget_exceeded) break;
    result_value = val;

    // Extract best action from TT for the root
    const StateHash64 root_hash = state.state_hash(false) ^
        (static_cast<StateHash64>(state.current_player()) * kGoldenRatio64);
    auto it = ctx.tt.find(root_hash);
    if (it != ctx.tt.end() && it->second.best_action >= 0) {
      best_action = it->second.best_action;
    }
  }

  const auto t1 = std::chrono::steady_clock::now();

  TailSolveResult result{};
  result.nodes_searched = ctx.nodes_searched;
  result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  result.best_action = best_action;
  result.value = result_value;
  result.budget_exceeded = ctx.budget_exceeded;

  if (!ctx.budget_exceeded) {
    if (result_value > 0.5f) {
      result.outcome = TailSolveOutcome::kProvenWin;
    } else if (result_value < -0.5f) {
      result.outcome = TailSolveOutcome::kProvenLoss;
    } else {
      result.outcome = TailSolveOutcome::kProvenDraw;
    }
  }

  return result;
}

}  // namespace board_ai::search
