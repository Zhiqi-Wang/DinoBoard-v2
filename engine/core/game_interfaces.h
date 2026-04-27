#pragma once

#include <memory>
#include <vector>

#include "types.h"

namespace board_ai {

class IGameState {
 public:
  virtual ~IGameState() = default;
  virtual std::unique_ptr<IGameState> clone_state() const = 0;
  virtual StateHash64 state_hash(bool include_hidden_rng) const = 0;
  virtual int current_player() const = 0;
  virtual bool is_terminal() const = 0;
  virtual int num_players() const = 0;
};

class IGameRules {
 public:
  virtual ~IGameRules() = default;
  virtual bool validate_action(const IGameState& state, ActionId action) const = 0;
  virtual std::vector<ActionId> legal_actions(const IGameState& state) const = 0;
  virtual UndoToken do_action_fast(IGameState& state, ActionId action) const = 0;
  virtual void undo_action(IGameState& state, const UndoToken& token) const = 0;
};

class IStateValueModel {
 public:
  virtual ~IStateValueModel() = default;
  virtual float terminal_value_for_player(const IGameState& state, int player) const = 0;
};

}  // namespace board_ai
