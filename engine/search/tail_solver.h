#pragma once

#include <cstdint>
#include <functional>

#include "../core/game_interfaces.h"

namespace board_ai::search {

using MarginScorer = std::function<float(const IGameState&, int)>;

struct TailSolveConfig {
  int depth_limit = 5;
  std::int64_t node_budget = 10'000'000;
  int time_limit_ms = 0;
  float margin_weight = 0.0f;
  MarginScorer margin_scorer;
};

enum class TailSolveOutcome {
  kUnknown = 0,
  kProvenWin = 1,
  kProvenLoss = 2,
  kProvenDraw = 3,
};

struct TailSolveResult {
  TailSolveOutcome outcome = TailSolveOutcome::kUnknown;
  ActionId best_action = -1;
  float value = 0.0f;
  std::int64_t nodes_searched = 0;
  double elapsed_ms = 0.0;
  bool budget_exceeded = false;
};

class ITailSolver {
 public:
  virtual ~ITailSolver() = default;
  virtual TailSolveResult solve(
      IGameState& state,
      const IGameRules& rules,
      const IStateValueModel& value_model,
      int perspective_player,
      const TailSolveConfig& config) const = 0;
};

class AlphaBetaTailSolver final : public ITailSolver {
 public:
  TailSolveResult solve(
      IGameState& state,
      const IGameRules& rules,
      const IStateValueModel& value_model,
      int perspective_player,
      const TailSolveConfig& config) const override;
};

}  // namespace board_ai::search
