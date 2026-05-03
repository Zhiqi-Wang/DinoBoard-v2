#pragma once

#include <any>
#include <map>
#include <random>
#include <string>

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

  // Serialize the tracker's internal belief to a canonical, comparable form.
  // Used by the AI API belief-equivalence tests: a self-play session and an
  // API session with different seeds must agree on belief after the same
  // action + event sequence. The returned map must be deterministic across
  // identical internal states — sort sets, use stable keys. The default
  // returns an empty map for trackers that hold no explicit state (Azul,
  // Coup — their belief is derived from game state by randomize_unseen).
  virtual std::map<std::string, std::any> serialize() const {
    return {};
  }
};

}  // namespace board_ai
