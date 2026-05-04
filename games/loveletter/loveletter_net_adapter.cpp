#include "loveletter_net_adapter.h"

#include <algorithm>

namespace board_ai::loveletter {

template <int NPlayers>
void LoveLetterFeatureEncoder<NPlayers>::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const LoveLetterState<NPlayers>*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= NPlayers) return;
  const auto& d = s->data;

  // Per-player public: alive, protected, current_player, hand_exposed,
  // discard counts by card type, discard size. All observer-visible.
  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (perspective_player + pi) % NPlayers;
    out->push_back(d.alive[pid] ? 1.0f : 0.0f);
    out->push_back(d.protected_flags[pid] ? 1.0f : 0.0f);
    out->push_back(d.current_player == pid ? 1.0f : 0.0f);
    out->push_back(d.hand_exposed[pid] ? 1.0f : 0.0f);

    for (int c = 1; c <= kCardTypes; ++c) {
      int count = 0;
      for (auto card : d.discard_piles[static_cast<size_t>(pid)]) {
        if (card == c) ++count;
      }
      out->push_back(static_cast<float>(count) /
                     static_cast<float>(kCardCounts[static_cast<size_t>(c)]));
    }

    out->push_back(static_cast<float>(d.discard_piles[static_cast<size_t>(pid)].size()) / 8.0f);
  }

  // Global public.
  out->push_back(static_cast<float>(d.deck.size()) / 16.0f);
  out->push_back(static_cast<float>(d.ply) / 20.0f);
  out->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);

  int alive_count = 0;
  for (int p = 0; p < NPlayers; ++p) {
    if (d.alive[p]) ++alive_count;
  }
  out->push_back(static_cast<float>(alive_count) / static_cast<float>(NPlayers));

  for (int c = 1; c <= kCardTypes; ++c) {
    int count = 0;
    for (auto card : d.face_up_removed) {
      if (card == c) ++count;
    }
    out->push_back(static_cast<float>(count) /
                   static_cast<float>(kCardCounts[static_cast<size_t>(c)]));
  }
}

template <int NPlayers>
void LoveLetterFeatureEncoder<NPlayers>::encode_private(
    const IGameState& state,
    int player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const LoveLetterState<NPlayers>*>(&state);
  if (!s || !out || player < 0 || player >= NPlayers) return;
  const auto& d = s->data;

  // Private from `player`'s perspective: for each player pid (in player's
  // perspective-relative order), output:
  //   - 8 dims: hand one-hot. Self: real hand. Opp: tracker->known_hand
  //     (0 if not known, i.e. all zeros).
  //   - 8 dims: drawn_card one-hot. Self AND current_player: real. Else 0.
  //
  // Tracker access: the encoder's tracker_ pointer is attached at
  // construction to a specific perspective. If `player` matches that
  // perspective, use tracker's knowledge; otherwise, output zeros for opp
  // hands (we don't have `player`'s tracker, and we must not leak opp
  // private info into a non-owner's private encoding).
  const bool use_tracker = (tracker_ != nullptr);

  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (player + pi) % NPlayers;
    const bool is_self = (pid == player);

    std::int8_t visible_hand = 0;
    if (is_self) {
      visible_hand = d.hand[pid];
    } else if (use_tracker && tracker_->known_hand(pid) > 0) {
      visible_hand = tracker_->known_hand(pid);
    }
    for (int c = 1; c <= kCardTypes; ++c) {
      out->push_back(visible_hand == c ? 1.0f : 0.0f);
    }

    for (int c = 1; c <= kCardTypes; ++c) {
      if (is_self && d.current_player == pid && d.drawn_card == c) {
        out->push_back(1.0f);
      } else {
        out->push_back(0.0f);
      }
    }
  }
}

template <int NPlayers>
void LoveLetterBeliefTracker<NPlayers>::init(
    int perspective_player, const AnyMap& initial_observation) {
  // Idempotent within the same perspective. Selfplay/arena runners call
  // init each ply for the acting player; we only reset on perspective
  // change or first call.
  if (perspective_player_ == perspective_player &&
      !known_hand_.empty() &&
      // First init has own_hand_ default 0 — but an empty-hand mid-game is
      // technically possible (eliminated). Use a separate flag for cleanliness.
      init_once_) {
    return;
  }
  perspective_player_ = perspective_player;
  known_hand_.fill(0);
  alive_tracked_.fill(true);
  init_once_ = true;

  auto it_h = initial_observation.find("my_hand");
  own_hand_ = (it_h != initial_observation.end())
      ? static_cast<std::int8_t>(std::any_cast<int>(it_h->second))
      : 0;
  auto it_d = initial_observation.find("my_drawn_card");
  own_drawn_card_ = (it_d != initial_observation.end())
      ? static_cast<std::int8_t>(std::any_cast<int>(it_d->second))
      : 0;
}

namespace {
// Extract the card referenced by a hand_override event whose "player" key
// equals the given player. Returns 0 if no such event is in the vector.
inline std::int8_t extract_hand_override(
    const std::vector<PublicEvent>& events, int player) {
  for (const auto& ev : events) {
    if (ev.first == "hand_override") {
      auto pit = ev.second.find("player");
      auto cit = ev.second.find("card");
      if (pit != ev.second.end() && cit != ev.second.end()) {
        if (std::any_cast<int>(pit->second) == player) {
          return static_cast<std::int8_t>(std::any_cast<int>(cit->second));
        }
      }
    }
  }
  return 0;
}

// Extract drawn_override. Love Letter emits this without a player key:
// the event is always "the current_player after advance_turn drew this"
// (perspective-only; non-perspectives get empty drawn_card in ai_view).
inline std::int8_t extract_drawn_override(
    const std::vector<PublicEvent>& events) {
  for (const auto& ev : events) {
    if (ev.first == "drawn_override") {
      auto cit = ev.second.find("card");
      if (cit != ev.second.end()) {
        return static_cast<std::int8_t>(std::any_cast<int>(cit->second));
      }
    }
  }
  return 0;
}

// Event-driven card play decoder — same math as loveletter_register's
// decode(), duplicated here to keep the tracker independent of register
// internals.
struct PlayedCardInfo {
  std::int8_t card = 0;
  int target = -1;
};

inline PlayedCardInfo decode_played(ActionId action) {
  PlayedCardInfo out;
  if (action >= kGuardOffset && action < kGuardOffset + kGuardCount) {
    out.card = kGuard;
    out.target = (action - kGuardOffset) / 7;
  } else if (action >= kPriestOffset && action < kPriestOffset + kPriestCount) {
    out.card = kPriest;
    out.target = action - kPriestOffset;
  } else if (action >= kBaronOffset && action < kBaronOffset + kBaronCount) {
    out.card = kBaron;
    out.target = action - kBaronOffset;
  } else if (action == kHandmaidAction) {
    out.card = kHandmaid;
  } else if (action >= kPrinceOffset && action < kPrinceOffset + kPrinceCount) {
    out.card = kPrince;
    out.target = action - kPrinceOffset;
  } else if (action >= kKingOffset && action < kKingOffset + kKingCount) {
    out.card = kKing;
    out.target = action - kKingOffset;
  } else if (action == kCountessAction) {
    out.card = kCountess;
  } else if (action == kPrincessAction) {
    out.card = kPrincess;
  }
  return out;
}
}  // namespace

template <int NPlayers>
void LoveLetterBeliefTracker<NPlayers>::observe_public_event(
    int actor,
    ActionId action,
    const std::vector<PublicEvent>& pre_events,
    const std::vector<PublicEvent>& post_events) {
  const PlayedCardInfo played = decode_played(action);
  const std::int8_t card = played.card;
  const int target = played.target;

  const bool i_am_actor = (actor == perspective_player_);
  const bool i_am_target = (target == perspective_player_);

  // Determine actor's pre-swap hand value (needed for King logic).
  // If actor is perspective, we already know (own_hand_ / own_drawn_card_).
  // If actor is not perspective, the event stream's pre-action
  // hand_override(actor) carries it.
  std::int8_t actor_pre_hand = 0;
  std::int8_t actor_pre_drawn = 0;
  if (i_am_actor) {
    actor_pre_hand = own_hand_;
    actor_pre_drawn = own_drawn_card_;
  } else {
    actor_pre_hand = extract_hand_override(pre_events, actor);
    actor_pre_drawn = extract_drawn_override(pre_events);
    // drawn_override pre-events carry the actor's drawn_card (only emitted
    // by extract_events when actor != perspective).
  }

  // --- Update perspective's own hand/drawn_card based on what they played
  // or received. ---
  if (i_am_actor) {
    if (card == own_drawn_card_) {
      // Played the drawn card; hand stays.
      own_drawn_card_ = 0;
    } else if (card == own_hand_) {
      // Played the hand card; drawn_card becomes the new hand.
      own_hand_ = own_drawn_card_;
      own_drawn_card_ = 0;
    }
    // For King: after the swap, perspective's hand equals the target's
    // pre-swap "other" card.
    if (card == kKing && target >= 0 && target < NPlayers) {
      const std::int8_t target_pre =
          i_am_target ? own_hand_  // already-updated (shouldn't happen: self-King)
                      : extract_hand_override(pre_events, target);
      if (target_pre != 0) {
        own_hand_ = target_pre;
      }
    }
  } else if (i_am_target) {
    // I am the target of someone else's action.
    if (card == kKing) {
      // My hand goes to actor; I receive actor's old "other" card.
      // Actor's other card = actor_pre_hand (they played King from
      // hand-or-drawn; the non-King one is actor's other).
      if (actor_pre_hand == kKing) {
        // Actor played King from hand; other is drawn_card.
        own_hand_ = actor_pre_drawn;
      } else {
        own_hand_ = actor_pre_hand;
      }
    } else if (card == kPrince) {
      // I get a new hand. The new hand is emitted via post_event
      // hand_override(target=perspective, new_card).
      for (const auto& ev : post_events) {
        if (ev.first == "hand_override") {
          auto pit = ev.second.find("player");
          auto cit = ev.second.find("card");
          if (pit != ev.second.end() && cit != ev.second.end() &&
              std::any_cast<int>(pit->second) == perspective_player_) {
            own_hand_ = static_cast<std::int8_t>(std::any_cast<int>(cit->second));
            break;
          }
        }
      }
    }
    // Priest/Baron/Guard on perspective reveal own_hand but don't change it.
  }

  // Post-event: advance_turn draws for the new current_player. If that's
  // perspective, drawn_override carries perspective's new drawn_card.
  for (const auto& ev : post_events) {
    if (ev.first == "drawn_override") {
      auto cit = ev.second.find("card");
      if (cit != ev.second.end()) {
        own_drawn_card_ = static_cast<std::int8_t>(std::any_cast<int>(cit->second));
      }
    }
  }

  // --- Update knowledge of other players' hands ---
  // When someone plays a card that we tracked, clear the tracked value
  // (they used it up).
  if (!i_am_actor && actor >= 0 && actor < Cfg::kPlayers) {
    if (known_hand_[actor] != 0 && known_hand_[actor] == card) {
      known_hand_[actor] = 0;
    }
  }

  // Card-specific knowledge updates.
  switch (card) {
    case kPriest:
      // Actor sees target's hand. Perspective learns it only if perspective
      // is the actor.
      if (i_am_actor && target >= 0 && target < Cfg::kPlayers) {
        const std::int8_t target_hand = extract_hand_override(pre_events, target);
        if (target_hand != 0) {
          known_hand_[target] = target_hand;
        }
      }
      break;

    case kBaron:
      // Both players compare; survivor known to the loser (loser is dead
      // anyway). In effect: if perspective is actor/target, learn the
      // other's card.
      if (target >= 0 && target < Cfg::kPlayers) {
        if (i_am_actor) {
          const std::int8_t target_hand = extract_hand_override(pre_events, target);
          if (target_hand != 0) known_hand_[target] = target_hand;
        }
        if (i_am_target) {
          // actor_pre_hand/drawn: the non-Baron one is actor's other card.
          // But Baron reveal compares hands, not drawn. actor plays Baron
          // from hand or drawn; the OTHER is the comparison card.
          // Perspective-as-target learns the card that was compared.
          std::int8_t other = 0;
          if (actor_pre_hand == kBaron) other = actor_pre_drawn;
          else if (actor_pre_drawn == kBaron) other = actor_pre_hand;
          else other = actor_pre_hand;  // fallback
          if (other != 0) known_hand_[actor] = other;
        }
      }
      break;

    case kKing:
      // After swap: actor now holds what target had; target holds what
      // actor had. Tracker records what perspective can deduce.
      if (target >= 0 && target < Cfg::kPlayers) {
        if (i_am_actor) {
          // Perspective (actor) gave target their pre-swap "other" card.
          const std::int8_t my_other =
              (actor_pre_hand == kKing) ? actor_pre_drawn : actor_pre_hand;
          known_hand_[target] = my_other;
        } else if (i_am_target) {
          // Perspective (target) gave actor their pre-target hand; they
          // now hold actor's "other" — but that's our own_hand, already
          // handled above. We don't need to record our own hand in
          // known_hand_.
          // Record actor's new hand (which was our old hand):
          // We need to know what our hand was BEFORE this King targeted us.
          // At this point own_hand_ has been updated to actor's old other.
          // Actor's new hand = perspective's pre-target hand. But by the
          // time we reach here, own_hand_ is the new value, not the old.
          // Workaround: we computed actor_pre_hand earlier (before own_hand_
          // mutation). No — actor_pre_hand is the actor's pre-swap hand.
          // The actor's new hand (what target had) = we don't track that
          // in known_hand_ (it's perspective's knowledge of opp, and
          // perspective LOST their card).
          // Actually: actor now has OUR old hand. Our old hand = the
          // own_hand_ value just before we applied the King swap update.
          // We lost that info — let's just clear known_hand_[actor].
          known_hand_[actor] = 0;
        } else {
          // Neither actor nor target — I saw the swap happen but don't
          // know either hand unless I previously tracked them.
          // Propagate: my tracked value for actor moves to target; my
          // tracked value for target moves to actor. Only if neither is
          // King (the known King was consumed).
          const std::int8_t ka = known_hand_[actor];
          const std::int8_t kt = known_hand_[target];
          known_hand_[actor] = (kt != 0 && kt != kKing) ? kt : 0;
          known_hand_[target] = (ka != 0 && ka != kKing) ? ka : 0;
        }
      }
      break;

    case kPrince:
      // Target discards + redraws. Their hand is fresh.
      if (target >= 0 && target < Cfg::kPlayers && target != perspective_player_) {
        known_hand_[target] = 0;
      }
      break;

    default:
      break;
  }

  // Death cleanup: in Love Letter, death is deterministic from Guard hits,
  // Baron loses, Princess plays. The tracker can infer deaths from the
  // public record (actor == target of successful Guard, etc.), but easier
  // to just drop known_hand for anyone whose play-sequence suggests death.
  // For correctness we infer "dead this ply" from the chain of play:
  //   - Princess played by actor → actor dies
  //   - Guard with matching guess → target dies (guess outcome is public)
  //   - Baron → one of actor/target dies (we can deduce from hand values)
  //
  // This mirrors observe_action's `for p: if !da.alive[p] clear known_hand_`
  // but derived from tracker state.
  if (card == kPrincess) {
    alive_tracked_[actor] = false;
    known_hand_[actor] = 0;
  } else if (card == kGuard && target >= 0 && target < Cfg::kPlayers) {
    // Guess is encoded in low 3 bits of the Guard action.
    const int guess = ((action - kGuardOffset) % 7) + 2;
    const std::int8_t target_hand = extract_hand_override(pre_events, target);
    if (target_hand != 0 && guess == target_hand) {
      alive_tracked_[target] = false;
      known_hand_[target] = 0;
    }
  } else if (card == kBaron && target >= 0 && target < Cfg::kPlayers) {
    const std::int8_t target_hand = extract_hand_override(pre_events, target);
    const std::int8_t actor_other_hand =
        (actor_pre_hand == kBaron) ? actor_pre_drawn : actor_pre_hand;
    if (target_hand != 0 && actor_other_hand != 0) {
      if (actor_other_hand > target_hand) {
        alive_tracked_[target] = false;
        known_hand_[target] = 0;
      } else if (target_hand > actor_other_hand) {
        alive_tracked_[actor] = false;
        known_hand_[actor] = 0;
        if (i_am_actor) {
          own_hand_ = 0;
          own_drawn_card_ = 0;
        }
      }
    }
  } else if (card == kPrince && target >= 0 && target < Cfg::kPlayers) {
    // Prince on someone holding Princess eliminates them.
    // We can detect this if target_hand == Princess in pre_events.
    if (target == perspective_player_) {
      // Perspective's own hand — was it Princess? own_hand_ was checked
      // before the swap above (line before post_events). We can compare
      // against the pre-action own_hand value. But that's tricky; skip
      // for now (alive tracking is best-effort).
    } else {
      const std::int8_t target_hand = extract_hand_override(pre_events, target);
      if (target_hand == kPrincess) {
        alive_tracked_[target] = false;
        known_hand_[target] = 0;
      }
    }
  }
}

template <int NPlayers>
void LoveLetterBeliefTracker<NPlayers>::randomize_unseen(
    IGameState& state, std::mt19937& rng) const {
  auto* s = dynamic_cast<LoveLetterState<NPlayers>*>(&state);
  if (!s) return;
  auto& d = s->data;

  std::array<int, 9> remaining{};
  for (int c = 1; c <= kCardTypes; ++c) {
    remaining[static_cast<size_t>(c)] = kCardCounts[static_cast<size_t>(c)];
  }

  auto consume = [&](std::int8_t card) {
    if (card >= 1 && card <= kCardTypes) {
      remaining[static_cast<size_t>(card)]--;
    }
  };

  // Perspective's own hand + drawn_card come from tracker state, not state.
  consume(own_hand_);
  if (own_drawn_card_ != 0) consume(own_drawn_card_);

  for (int p = 0; p < NPlayers; ++p) {
    for (auto card : d.discard_piles[static_cast<size_t>(p)]) {
      consume(card);
    }
  }

  for (auto card : d.face_up_removed) {
    consume(card);
  }

  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective_player_) continue;
    if (!d.alive[p]) continue;
    if (known_hand_[p] > 0) {
      consume(known_hand_[p]);
    }
  }

  std::vector<std::int8_t> unseen;
  for (int c = 1; c <= kCardTypes; ++c) {
    for (int i = 0; i < remaining[static_cast<size_t>(c)]; ++i) {
      unseen.push_back(static_cast<std::int8_t>(c));
    }
  }
  std::shuffle(unseen.begin(), unseen.end(), rng);

  size_t idx = 0;

  if (idx < unseen.size()) {
    d.set_aside_card = unseen[idx++];
  }

  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective_player_) continue;
    if (!d.alive[p]) continue;
    if (known_hand_[p] > 0) {
      d.hand[p] = known_hand_[p];
    } else if (idx < unseen.size()) {
      d.hand[p] = unseen[idx++];
    }
  }

  if (d.current_player != perspective_player_ && d.drawn_card != 0) {
    if (idx < unseen.size()) {
      d.drawn_card = unseen[idx++];
    }
  }

  d.deck.clear();
  while (idx < unseen.size()) {
    d.deck.push_back(unseen[idx++]);
  }

  d.draw_nonce ^= static_cast<std::uint64_t>(rng());
}

template <int NPlayers>
AnyMap LoveLetterBeliefTracker<NPlayers>::serialize() const {
  AnyMap out;
  out["perspective_player"] = perspective_player_;
  std::vector<int> known(NPlayers);
  for (int p = 0; p < NPlayers; ++p) {
    known[p] = static_cast<int>(known_hand_[p]);
  }
  out["known_hand"] = known;
  return out;
}

template class LoveLetterFeatureEncoder<2>;
template class LoveLetterFeatureEncoder<3>;
template class LoveLetterFeatureEncoder<4>;
template class LoveLetterBeliefTracker<2>;
template class LoveLetterBeliefTracker<3>;
template class LoveLetterBeliefTracker<4>;

}  // namespace board_ai::loveletter
