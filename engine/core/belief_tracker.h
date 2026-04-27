#pragma once

#include <random>

#include "game_interfaces.h"

namespace board_ai {

class IBeliefTracker {
 public:
  virtual ~IBeliefTracker() = default;

  // Initialize from game start, recording initially visible information.
  virtual void init(const IGameState& state, int perspective_player) = 0;

  // Update after each action: record any newly revealed information.
  virtual void observe_action(
      const IGameState& state_before,
      ActionId action,
      const IGameState& state_after) = 0;

  // Randomize all unseen information in-place for MCTS determinization.
  // Uses the tracked seen-set to build the unseen pool, then randomly
  // distributes it (e.g. deck + opponent hidden cards).
  virtual void randomize_unseen(IGameState& state, std::mt19937& rng) const = 0;
};

}  // namespace board_ai
