#pragma once

#include <any>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace board_ai {

// ---- Event protocol primitives (used by IBeliefTracker and game bundle) ----
// AnyMap: game-specific payload for events and observations. Keys are
// stable string names chosen by the game; values are wrapped in std::any
// so different games can carry different payload types.
using AnyMap = std::map<std::string, std::any>;

// Phase of a public event relative to the action it describes:
//   kPreAction  — the event describes hidden info the action depends on
//                 (e.g. Baron target's hand in Love Letter). Applied BEFORE
//                 rules.apply() during event replay.
//   kPostAction — the event describes a random outcome the action produced
//                 (e.g. Splendor deck flip). Applied AFTER rules.apply().
enum class EventPhase { kPreAction = 0, kPostAction = 1 };

// A single public event: (kind, payload). Kind is a stable string chosen by
// the game (e.g. "deck_flip", "hand_override"). Payload keys/values are
// game-defined.
using PublicEvent = std::pair<std::string, AnyMap>;

// Events for a single action, split by phase.
struct PublicEventTrace {
  std::vector<PublicEvent> pre_events{};
  std::vector<PublicEvent> post_events{};
};

class IGameState {
 public:
  virtual ~IGameState() = default;
  virtual std::unique_ptr<IGameState> clone_state() const = 0;
  virtual void copy_from(const IGameState& other) = 0;

  // Legacy full-state hash. Retained for non-ISMCTS paths (tail solver,
  // transposition debug). New code paths use state_hash_for_perspective.
  virtual StateHash64 state_hash(bool include_hidden_rng) const = 0;

  // ========== Public/private hash API ==========
  //
  // Game declares which fields are public and which are private per-player.
  // Framework derives state_hash_for_perspective(p) by combining:
  //   step_count, then hash_public_fields(h), then hash_private_fields(p, h).
  //
  // Contract:
  //   - hash_public_fields: hash every field that ALL players can see
  //   - hash_private_fields(p): hash every field that ONLY player p can see
  //     (plus p's own knowledge/belief-derived info that p is allowed to have)
  //   - Do NOT hash fields belonging to players other than p in
  //     hash_private_fields. Do NOT hash private fields in hash_public_fields.
  //
  // The same partition drives MCTS node keying, encoder feature extraction,
  // and the AI-pipeline-no-leak test suite. Keeping the two methods aligned
  // with encoder scope is the game author's responsibility; the framework
  // enforces the partition is well-defined via hash tests.
  virtual void hash_public_fields(Hasher& h) const = 0;
  virtual void hash_private_fields(int player, Hasher& h) const = 0;

  // Framework-provided perspective hash. Combines step_count (for DAG
  // acyclicity) + public fields + given player's private fields. NOT
  // virtual — games override the two helpers above, not this.
  StateHash64 state_hash_for_perspective(int player) const {
    Hasher h;
    h.add(step_count_);
    hash_public_fields(h);
    hash_private_fields(player, h);
    return h.finalize();
  }

  // ========== Framework-provided step counter ==========
  //
  // Monotonically increasing across do_action_fast calls. Guarantees DAG
  // acyclicity in MCTS: any two states along a sim path have distinct
  // step_count → distinct hash (assuming hash_public_fields includes it,
  // which state_hash_for_perspective does automatically).
  //
  // Games MUST NOT touch step_count directly. The framework bumps it at the
  // start of do_action_fast via `begin_step()` and rolls it back in
  // undo_action via `end_step()`. Games call those helpers from inside
  // their overrides.
  std::uint32_t step_count() const { return step_count_; }
  void begin_step() { ++step_count_; }
  void end_step() { --step_count_; }

  virtual int current_player() const = 0;
  virtual int first_player() const { return 0; }
  virtual bool is_terminal() const = 0;
  virtual int num_players() const = 0;
  virtual int winner() const = 0;
  virtual bool is_turn_start() const { return true; }
  virtual void reset_with_seed(std::uint64_t seed) = 0;

 protected:
  // Counter for DAG acyclicity. Framework-managed. Reset to 0 in
  // reset_with_seed implementations; bumped by begin_step; rolled back by
  // end_step.
  std::uint32_t step_count_ = 0;
};

template <typename Derived>
class CloneableState : public IGameState {
 public:
  std::unique_ptr<IGameState> clone_state() const override {
    return std::make_unique<Derived>(static_cast<const Derived&>(*this));
  }
  void copy_from(const IGameState& other) override {
    static_cast<Derived&>(*this) = static_cast<const Derived&>(other);
  }
};

template <typename ConcreteState>
const ConcreteState& checked_cast(const IGameState& state) {
  const auto* p = dynamic_cast<const ConcreteState*>(&state);
  if (!p) throw std::invalid_argument("unexpected IGameState subtype");
  return *p;
}

template <typename ConcreteState>
ConcreteState& checked_cast(IGameState& state) {
  auto* p = dynamic_cast<ConcreteState*>(&state);
  if (!p) throw std::invalid_argument("unexpected IGameState subtype");
  return *p;
}

class IGameRules {
 public:
  virtual ~IGameRules() = default;
  virtual bool validate_action(const IGameState& state, ActionId action) const = 0;
  virtual std::vector<ActionId> legal_actions(const IGameState& state) const = 0;
  virtual UndoToken do_action_fast(IGameState& state, ActionId action) const = 0;
  virtual void undo_action(IGameState& state, const UndoToken& token) const = 0;

  // `do_action_deterministic` is used by the tail solver path: same as
  // `do_action_fast` but must NEVER draw from hidden sources (deck, bag,
  // opp hand). Games with physical randomness in do_action_fast should
  // override — e.g. Splendor uses `forced_draw_override = -2` sentinel
  // to mean "freeze the random source". See docs/GAME_DEVELOPMENT_GUIDE §9.2.
  virtual UndoToken do_action_deterministic(IGameState& state, ActionId action) const {
    return do_action_fast(state, action);
  }
};

class IStateValueModel {
 public:
  virtual ~IStateValueModel() = default;
  virtual float terminal_value_for_player(const IGameState& state, int player) const = 0;

  virtual std::vector<float> terminal_values(const IGameState& state) const {
    const int n = state.num_players();
    std::vector<float> v(static_cast<size_t>(n));
    for (int p = 0; p < n; ++p) {
      v[static_cast<size_t>(p)] = terminal_value_for_player(state, p);
    }
    return v;
  }
};

class DefaultStateValueModel final : public IStateValueModel {
 public:
  float terminal_value_for_player(const IGameState& state, int player) const override {
    if (!state.is_terminal()) return 0.0f;
    const int w = state.winner();
    if (w < 0) return 0.0f;
    const int n = state.num_players();
    if (n <= 2) return w == player ? 1.0f : -1.0f;
    return w == player ? 1.0f : -1.0f / static_cast<float>(n - 1);
  }
};

}  // namespace board_ai
