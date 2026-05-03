#include <stdexcept>
#include <string>
#include <vector>

#include "../../engine/core/game_registry.h"
#include "loveletter_state.h"
#include "loveletter_rules.h"
#include "loveletter_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::ActionId;
using board_ai::IGameState;
using board_ai::EventPhase;
using board_ai::PublicEvent;
using board_ai::PublicEventTrace;

static const char* const kCardNames[9] = {
    "", "Guard", "Priest", "Baron", "Handmaid", "Prince", "King", "Countess", "Princess"};

template <int NPlayers>
AnyMap serialize_loveletter(const IGameState& state) {
  using namespace board_ai::loveletter;
  const auto& s = board_ai::checked_cast<LoveLetterState<NPlayers>>(state);
  const auto& d = s.data;

  AnyMap m;
  m["current_player"] = std::any(state.current_player());
  m["is_terminal"] = std::any(state.is_terminal());
  m["winner"] = std::any(state.winner());
  m["num_players"] = std::any(NPlayers);
  m["ply"] = std::any(d.ply);
  m["deck_size"] = std::any(static_cast<int>(d.deck.size()));

  std::vector<AnyMap> players;
  for (int p = 0; p < NPlayers; ++p) {
    AnyMap pm;
    pm["alive"] = std::any(static_cast<bool>(d.alive[p]));
    pm["protected"] = std::any(static_cast<bool>(d.protected_flags[p]));
    pm["hand"] = std::any(static_cast<int>(d.hand[p]));
    pm["hand_name"] = std::any(std::string(
        d.hand[p] >= 1 && d.hand[p] <= 8 ? kCardNames[d.hand[p]] : ""));

    std::vector<int> discards;
    for (auto c : d.discard_piles[static_cast<size_t>(p)]) {
      discards.push_back(static_cast<int>(c));
    }
    pm["discards"] = std::any(discards);

    players.push_back(std::move(pm));
  }
  m["players"] = std::any(players);

  m["drawn_card"] = std::any(static_cast<int>(d.drawn_card));
  m["drawn_card_name"] = std::any(std::string(
      d.drawn_card >= 1 && d.drawn_card <= 8 ? kCardNames[d.drawn_card] : ""));

  std::vector<int> face_up;
  for (auto c : d.face_up_removed) face_up.push_back(static_cast<int>(c));
  m["face_up_removed"] = std::any(face_up);

  return m;
}

AnyMap describe_loveletter(ActionId action) {
  using namespace board_ai::loveletter;
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));

  if (action >= kGuardOffset && action < kGuardOffset + kGuardCount) {
    int idx = action - kGuardOffset;
    int target = idx / 7;
    int guess = idx % 7 + 2;
    m["type"] = std::any(std::string("guard"));
    m["card"] = std::any(1);
    m["card_name"] = std::any(std::string("Guard"));
    m["target"] = std::any(target);
    m["guess"] = std::any(guess);
    m["guess_name"] = std::any(std::string(
        guess >= 1 && guess <= 8 ? kCardNames[guess] : ""));
  } else if (action >= kPriestOffset && action < kPriestOffset + kPriestCount) {
    m["type"] = std::any(std::string("priest"));
    m["card"] = std::any(2);
    m["card_name"] = std::any(std::string("Priest"));
    m["target"] = std::any(action - kPriestOffset);
  } else if (action >= kBaronOffset && action < kBaronOffset + kBaronCount) {
    m["type"] = std::any(std::string("baron"));
    m["card"] = std::any(3);
    m["card_name"] = std::any(std::string("Baron"));
    m["target"] = std::any(action - kBaronOffset);
  } else if (action == kHandmaidAction) {
    m["type"] = std::any(std::string("handmaid"));
    m["card"] = std::any(4);
    m["card_name"] = std::any(std::string("Handmaid"));
  } else if (action >= kPrinceOffset && action < kPrinceOffset + kPrinceCount) {
    m["type"] = std::any(std::string("prince"));
    m["card"] = std::any(5);
    m["card_name"] = std::any(std::string("Prince"));
    m["target"] = std::any(action - kPrinceOffset);
  } else if (action >= kKingOffset && action < kKingOffset + kKingCount) {
    m["type"] = std::any(std::string("king"));
    m["card"] = std::any(6);
    m["card_name"] = std::any(std::string("King"));
    m["target"] = std::any(action - kKingOffset);
  } else if (action == kCountessAction) {
    m["type"] = std::any(std::string("countess"));
    m["card"] = std::any(7);
    m["card_name"] = std::any(std::string("Countess"));
  } else if (action == kPrincessAction) {
    m["type"] = std::any(std::string("princess"));
    m["card"] = std::any(8);
    m["card_name"] = std::any(std::string("Princess"));
  }

  return m;
}

board_ai::HeuristicResult heuristic_random(
    board_ai::IGameState& state, const board_ai::IGameRules& rules, std::uint64_t /*rng_seed*/) {
  auto legal = rules.legal_actions(state);
  board_ai::HeuristicResult result;
  result.actions = legal;
  result.scores.assign(legal.size(), 1.0);
  return result;
}

// --- Public-event protocol -------------------------------------------------
//
// Love Letter's do_action_fast reads hidden state for several card effects:
//   Guard     — reads target's hand to check the guess
//   Priest    — exposes target's hand (tracker reads it post-action)
//   Baron     — compares actor's and target's hands
//   Prince    — discards target's hand (revealed) and redraws
//   King      — swaps actor's and target's hands
//
// For each, we emit a pre-action `hand_override` event to set AI's internal
// state.hand[target] to GT's value before do_action_fast runs. Without this,
// the action resolves differently in AI vs GT (wrong elimination, wrong
// Priest reveal, etc.) and the belief tracker records garbage.
//
// Post-action: advance_turn draws a card for the next alive player. If the
// new current_player is the traced perspective, we emit a `drawn_override`
// event so AI knows what it actually drew (instead of its own random draw
// from a different internal deck). Prince also redraws for the target; if
// the target is perspective, we emit a `hand_override` post-event for them.

namespace loveletter_events {

using board_ai::loveletter::LoveLetterConfig;
using board_ai::loveletter::LoveLetterState;
using board_ai::loveletter::LoveLetterData;
using board_ai::loveletter::kGuard;
using board_ai::loveletter::kPriest;
using board_ai::loveletter::kBaron;
using board_ai::loveletter::kHandmaid;
using board_ai::loveletter::kPrince;
using board_ai::loveletter::kKing;
using board_ai::loveletter::kCountess;
using board_ai::loveletter::kPrincess;
using board_ai::loveletter::kGuardOffset;
using board_ai::loveletter::kGuardCount;
using board_ai::loveletter::kPriestOffset;
using board_ai::loveletter::kPriestCount;
using board_ai::loveletter::kBaronOffset;
using board_ai::loveletter::kBaronCount;
using board_ai::loveletter::kHandmaidAction;
using board_ai::loveletter::kPrinceOffset;
using board_ai::loveletter::kPrinceCount;
using board_ai::loveletter::kKingOffset;
using board_ai::loveletter::kKingCount;
using board_ai::loveletter::kCountessAction;
using board_ai::loveletter::kPrincessAction;

struct DecodedAction {
  std::int8_t card = 0;
  int target = -1;
  std::int8_t guess = 0;
};

DecodedAction decode(ActionId action) {
  DecodedAction out;
  if (action >= kGuardOffset && action < kGuardOffset + kGuardCount) {
    out.card = kGuard;
    int idx = action - kGuardOffset;
    out.target = idx / 7;
    out.guess = static_cast<std::int8_t>((idx % 7) + 2);
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

// Does this action's do_action_fast read d.hand[target] in a way that
// depends on the target's actual card value? (Used to decide whether to
// emit a pre-action hand_override event for the target.)
bool action_reads_target_hand(std::int8_t card) {
  return card == kGuard || card == kPriest || card == kBaron ||
         card == kPrince || card == kKing;
}

template <int NPlayers>
AnyMap extract_initial_observation(const IGameState& state, int perspective) {
  const auto& s = board_ai::checked_cast<LoveLetterState<NPlayers>>(state);
  const auto& d = s.data;
  AnyMap out;
  // Perspective's own starting hand (visible to them).
  if (perspective >= 0 && perspective < NPlayers) {
    out["my_hand"] = std::any(static_cast<int>(d.hand[perspective]));
  }
  // If perspective is the starting current_player, they've already drawn
  // a card at game start — visible to them.
  if (d.current_player == perspective) {
    out["my_drawn_card"] = std::any(static_cast<int>(d.drawn_card));
  } else {
    out["my_drawn_card"] = std::any(0);
  }
  // In 2p, 3 cards are face-up removed; public to all.
  std::vector<int> face_up;
  for (auto c : d.face_up_removed) face_up.push_back(static_cast<int>(c));
  out["face_up_removed"] = std::any(face_up);
  return out;
}

// Reset AI's internal state to be consistent with the initial observation:
// perspective's hand is set from `my_hand`, other players' hands are random
// consistent with the remaining card counts. The belief tracker gets
// re-init'd by the caller.
template <int NPlayers>
void apply_initial_observation(IGameState& state, int perspective, const AnyMap& obs) {
  using board_ai::loveletter::kCardCounts;
  using board_ai::loveletter::kCardTypes;
  auto& s = board_ai::checked_cast<LoveLetterState<NPlayers>>(state);
  auto& d = s.data;

  auto it_hand = obs.find("my_hand");
  if (it_hand == obs.end()) {
    throw std::runtime_error("loveletter initial_observation missing 'my_hand'");
  }
  const std::int8_t my_hand = static_cast<std::int8_t>(std::any_cast<int>(it_hand->second));

  // drawn_card (0 if perspective isn't current_player at game start).
  std::int8_t my_drawn = 0;
  auto it_draw = obs.find("my_drawn_card");
  if (it_draw != obs.end()) {
    my_drawn = static_cast<std::int8_t>(std::any_cast<int>(it_draw->second));
  }

  std::vector<int> face_up_v;
  auto it_fu = obs.find("face_up_removed");
  if (it_fu != obs.end()) {
    face_up_v = std::any_cast<std::vector<int>>(it_fu->second);
  }

  // Pool of remaining cards = full deck minus face_up minus my_hand.
  std::array<int, 9> remaining{};
  for (int c = 1; c <= kCardTypes; ++c) {
    remaining[c] = kCardCounts[c];
  }
  for (int c : face_up_v) {
    if (c >= 1 && c <= kCardTypes) remaining[c]--;
  }
  if (my_hand >= 1 && my_hand <= kCardTypes) remaining[my_hand]--;
  if (my_drawn >= 1 && my_drawn <= kCardTypes) remaining[my_drawn]--;

  // Rebuild face_up_removed from observation.
  d.face_up_removed.clear();
  for (int c : face_up_v) d.face_up_removed.push_back(static_cast<std::int8_t>(c));

  // Seed each other player's hand with ANY remaining card (doesn't matter
  // which — AI's belief is "unknown"; randomize_unseen will re-sample when
  // called). Just pick the first available to keep things deterministic.
  auto take_one = [&](int exclude = -1) -> std::int8_t {
    for (int c = 1; c <= kCardTypes; ++c) {
      if (c == exclude) continue;
      if (remaining[c] > 0) {
        remaining[c]--;
        return static_cast<std::int8_t>(c);
      }
    }
    return 0;
  };
  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective) {
      d.hand[p] = my_hand;
    } else {
      d.hand[p] = take_one();
    }
    d.alive[p] = 1;
    d.protected_flags[p] = 0;
    d.hand_exposed[p] = 0;
    d.discard_piles[p].clear();
  }
  d.set_aside_card = take_one();
  // Rebuild deck from what's left.
  d.deck.clear();
  for (int c = 1; c <= kCardTypes; ++c) {
    for (int i = 0; i < remaining[c]; ++i) {
      d.deck.push_back(static_cast<std::int8_t>(c));
    }
  }
  // drawn_card: if perspective is the starting current_player, we know it.
  // Otherwise assign a random placeholder (consistent-by-count; the actual
  // value is hidden from AI and will be corrected by events when it matters).
  if (d.current_player == perspective) {
    d.drawn_card = my_drawn;
  } else {
    d.drawn_card = take_one();
  }
}

template <int NPlayers>
PublicEventTrace extract_events(
    const IGameState& before,
    ActionId action,
    const IGameState& after,
    int perspective) {
  const auto& sb = board_ai::checked_cast<LoveLetterState<NPlayers>>(before);
  const auto& sa = board_ai::checked_cast<LoveLetterState<NPlayers>>(after);
  const auto& db = sb.data;
  const auto& da = sa.data;
  PublicEventTrace out;

  const auto decoded = decode(action);
  const int actor = db.current_player;
  const int target = decoded.target;
  const std::int8_t card = decoded.card;

  // Pre-event: override target's hand so the action resolves correctly.
  // Only needed when target != perspective (perspective's hand is already
  // known to AI) AND the action actually reads target's hand.
  if (action_reads_target_hand(card) &&
      target >= 0 && target < NPlayers && target != perspective &&
      db.alive[target] && !db.protected_flags[target]) {
    AnyMap payload;
    payload["player"] = std::any(target);
    payload["card"] = std::any(static_cast<int>(db.hand[target]));
    out.pre_events.emplace_back("hand_override", std::move(payload));
  }

  // Baron special case: if target is perspective, actor's hand will be
  // revealed (to perspective's tracker) via the outcome. Emit pre-event
  // for actor so AI's state has actor's real hand at comparison time.
  if (card == kBaron && target == perspective && actor != perspective &&
      db.alive[actor] && !db.protected_flags[actor]) {
    AnyMap payload;
    payload["player"] = std::any(actor);
    payload["card"] = std::any(static_cast<int>(db.hand[actor]));
    out.pre_events.emplace_back("hand_override", std::move(payload));
  }

  // Post-event 1: Prince target redraws. If target == perspective,
  // perspective learns their new hand.
  if (card == kPrince && target == perspective &&
      db.alive[target] && !db.protected_flags[target] &&
      da.alive[target]) {
    AnyMap payload;
    payload["player"] = std::any(static_cast<int>(target));
    payload["card"] = std::any(static_cast<int>(da.hand[target]));
    out.post_events.emplace_back("hand_override", std::move(payload));
  }

  // Post-event 2: advance_turn draws for the NEW current player. If that
  // player is perspective, emit the drawn card so AI's d.drawn_card matches.
  if (!da.terminal && da.current_player == perspective &&
      da.drawn_card != 0) {
    AnyMap payload;
    payload["card"] = std::any(static_cast<int>(da.drawn_card));
    out.post_events.emplace_back("drawn_override", std::move(payload));
  }

  return out;
}

template <int NPlayers>
void apply_event(IGameState& state, EventPhase /*phase*/,
                 const std::string& kind, const AnyMap& payload) {
  auto& s = board_ai::checked_cast<LoveLetterState<NPlayers>>(state);
  auto& d = s.data;
  if (kind == "hand_override") {
    const int player = std::any_cast<int>(payload.at("player"));
    const int card = std::any_cast<int>(payload.at("card"));
    if (player < 0 || player >= NPlayers) {
      throw std::runtime_error("loveletter hand_override: bad player");
    }
    d.hand[player] = static_cast<std::int8_t>(card);
  } else if (kind == "drawn_override") {
    const int card = std::any_cast<int>(payload.at("card"));
    d.drawn_card = static_cast<std::int8_t>(card);
  } else {
    throw std::runtime_error("loveletter: unknown event kind '" + kind + "'");
  }
}

}  // namespace loveletter_events

template <int NPlayers>
board_ai::GameBundle make_loveletter(const std::string& game_id, std::uint64_t seed) {
  using namespace board_ai::loveletter;
  board_ai::GameBundle b;
  b.game_id = game_id;
  auto s = std::make_unique<LoveLetterState<NPlayers>>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<LoveLetterRules<NPlayers>>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  auto tracker = std::make_unique<LoveLetterBeliefTracker<NPlayers>>();
  b.encoder = std::make_unique<LoveLetterFeatureEncoder<NPlayers>>(tracker.get());
  b.belief_tracker = std::move(tracker);
  b.stochastic_detector = board_ai::default_stochastic_detector;
  b.state_serializer = serialize_loveletter<NPlayers>;
  b.action_descriptor = describe_loveletter;
  b.heuristic_picker = heuristic_random;
  b.public_event_extractor = loveletter_events::extract_events<NPlayers>;
  b.public_event_applier = loveletter_events::apply_event<NPlayers>;
  b.initial_observation_extractor = loveletter_events::extract_initial_observation<NPlayers>;
  b.initial_observation_applier = loveletter_events::apply_initial_observation<NPlayers>;
  return b;
}

board_ai::GameRegistrar reg_loveletter("loveletter", [](std::uint64_t seed) {
  return make_loveletter<2>("loveletter", seed);
});
board_ai::GameRegistrar reg_loveletter_2p("loveletter_2p", [](std::uint64_t seed) {
  return make_loveletter<2>("loveletter_2p", seed);
});
board_ai::GameRegistrar reg_loveletter_3p("loveletter_3p", [](std::uint64_t seed) {
  return make_loveletter<3>("loveletter_3p", seed);
});
board_ai::GameRegistrar reg_loveletter_4p("loveletter_4p", [](std::uint64_t seed) {
  return make_loveletter<4>("loveletter_4p", seed);
});

}  // namespace
