#pragma once

#include <any>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "game_interfaces.h"

namespace board_ai {

// Observer's memory of a game in progress.
//
// Design principle: the tracker's inputs come SOLELY through this
// interface — initial observations at session start, then public events
// for each action. It never receives a reference to the game state for
// reading (randomize_unseen takes a state for WRITING only). This mirrors
// the AI API contract: a tracker running behind an AI/player boundary
// could be driven by action_ids + event payloads over the wire.
//
// Concretely, this means:
//   - observe_public_event MUST update internal belief from the given
//     payloads — no peeking at truth or any other state.
//   - init is driven by initial_observation (not state); the game's
//     initial_observation_extractor produces observer-visible starting
//     info (own hand, public tableau, etc.).
//   - Tracker implementations are typed per game; they use their internal
//     belief state plus event payloads to answer encoder queries.
//
// Enforced by tests:
//   - test_api_belief_matches_selfplay — a tracker initialized with a
//     different seed than ground truth must converge to the same belief
//     after replaying the observation stream. Any hidden state read would
//     diverge.
//   - test_api_mcts_policy_invariance — MCTS policy depending on tracker
//     belief must be identical across selfplay and API paths.
class IBeliefTracker {
 public:
  virtual ~IBeliefTracker() = default;

  // Initialize at game start from the observer's initial observation.
  // `initial_observation` is produced by the game's
  // initial_observation_extractor for the perspective player — it carries
  // only observer-visible info (perspective's own starting hand, public
  // setup). No state reference is passed.
  virtual void init(int perspective_player,
                    const AnyMap& initial_observation) = 0;

  // Update after each action using ONLY the public event stream.
  //
  //   actor: the player whose action was taken
  //   action: the ActionId taken by `actor`
  //   pre_events: events describing hidden info the action depends on
  //       (e.g. Baron target's hand reveal in Love Letter). Conceptually
  //       applied BEFORE the action during event replay.
  //   post_events: events describing random outcomes or post-effects the
  //       action produced (e.g. Splendor deck flip). Conceptually applied
  //       AFTER the action.
  //
  // Tracker must update its belief state solely from these payloads.
  // Payload kinds and keys are defined by the game in
  // public_event_extractor.
  virtual void observe_public_event(
      int actor,
      ActionId action,
      const std::vector<PublicEvent>& pre_events,
      const std::vector<PublicEvent>& post_events) = 0;

  // Randomize all unseen information in-place for MCTS determinization.
  // Uses the tracked belief (built from init + observe_public_event) to
  // build the unseen pool, then randomly distributes it (e.g. deck +
  // opponent hidden cards).
  //
  // This method WRITES to `state`. It may READ observer-visible fields
  // from `state` (e.g. alive flags, discard piles) to compute WHAT to
  // randomize, but it MUST NOT READ hidden fields that the observer
  // doesn't know. Callers pass the observer view (ai_view), so hidden
  // fields in `state` are placeholders anyway — but tracker code should
  // not rely on that and should derive all belief information from its
  // own accumulated observations.
  virtual void randomize_unseen(IGameState& state, std::mt19937& rng) const = 0;

  // Serialize the tracker's internal belief to a canonical, comparable form.
  // Used by the AI API belief-equivalence tests: a self-play session and an
  // API session with different seeds must agree on belief after the same
  // action + event sequence. The returned map must be deterministic across
  // identical internal states — sort sets, use stable keys. The default
  // returns an empty map for trackers that hold no explicit state.
  virtual AnyMap serialize() const { return {}; }

  // Reconcile public state fields after applying an observation. Called at
  // the end of apply_observation in the API path (py_engine), AFTER the
  // event applier has done its per-event work. Gives the tracker a chance
  // to fix invariants that per-event appliers can't maintain locally —
  // notably, Splendor's deck content is a function of (tableau, all
  // reserved, bought cards), and per-slot deck_flip events conflate
  // "slot shift" with "real deck draw" in a way that drifts API state.
  //
  // The default is a no-op; games whose event protocol maintains a clean
  // local invariant don't need to override it. Splendor overrides to
  // recompute deck content from tracker's seen_cards, so |deck_tier_t|
  // matches GT's exactly after every observation.
  //
  // May READ / WRITE observer-visible fields in state. Must NOT read
  // hidden fields (use tracker's internal knowledge instead).
  virtual void reconcile_state(IGameState& /*state*/) const {}
};

}  // namespace board_ai
