#include <stdexcept>
#include <string>
#include <vector>

#include "../../engine/core/game_registry.h"
#include "coup_state.h"
#include "coup_rules.h"
#include "coup_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::ActionId;
using board_ai::IGameState;
using board_ai::EventPhase;
using board_ai::PublicEvent;
using board_ai::PublicEventTrace;

static const char* const kCharNames[5] = {
    "Duke", "Assassin", "Captain", "Ambassador", "Contessa"};

template <int NPlayers>
AnyMap serialize_coup(const IGameState& state) {
  using namespace board_ai::coup;
  const auto& s = board_ai::checked_cast<CoupState<NPlayers>>(state);
  const auto& d = s.data;

  AnyMap m;
  m["current_player"] = std::any(state.current_player());
  m["is_terminal"] = std::any(state.is_terminal());
  m["winner"] = std::any(state.winner());
  m["num_players"] = std::any(NPlayers);
  m["ply"] = std::any(d.ply);
  m["stage"] = std::any(static_cast<int>(d.stage));
  m["active_player"] = std::any(d.active_player);
  m["declared_action"] = std::any(static_cast<int>(d.declared_action));
  m["action_target"] = std::any(d.action_target);
  m["blocker"] = std::any(d.blocker);
  m["challenger"] = std::any(d.challenger);
  m["deck_size"] = std::any(static_cast<int>(d.court_deck.size()));

  // Exchange-drawn cards (Ambassador). Only exposed during the two
  // exchange-return stages; outside those stages the field is
  // default-zero and should not be confused with an actual drawn Duke.
  // -1 means "slot already emptied during Return1". Frontend renders
  // these as extra cards in the active player's hand during the return
  // stages so the player sees 2 hand + 2 drawn = 4 cards and can click
  // one to return.
  std::vector<int> exchange_drawn;
  if (d.stage == CoupStage::kExchangeReturn1 ||
      d.stage == CoupStage::kExchangeReturn2) {
    exchange_drawn.reserve(2);
    for (int i = 0; i < 2; ++i) {
      exchange_drawn.push_back(static_cast<int>(d.exchange_drawn[i]));
    }
  }
  m["exchange_drawn"] = std::any(exchange_drawn);

  std::vector<AnyMap> players;
  for (int p = 0; p < NPlayers; ++p) {
    AnyMap pm;
    pm["alive"] = std::any(static_cast<bool>(d.alive[p]));
    pm["coins"] = std::any(d.coins[p]);

    std::vector<AnyMap> influences;
    for (int sl = 0; sl < 2; ++sl) {
      AnyMap inf;
      inf["character"] = std::any(static_cast<int>(d.influence[p][sl]));
      inf["character_name"] = std::any(std::string(
          d.influence[p][sl] >= 0 && d.influence[p][sl] < 5
              ? kCharNames[d.influence[p][sl]] : ""));
      inf["revealed"] = std::any(static_cast<bool>(d.revealed[p][sl]));
      influences.push_back(std::move(inf));
    }
    pm["influences"] = std::any(influences);

    players.push_back(std::move(pm));
  }
  m["players"] = std::any(players);

  return m;
}

AnyMap describe_coup(ActionId action) {
  using namespace board_ai::coup;
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));

  if (action == kIncomeAction) {
    m["type"] = std::any(std::string("income"));
  } else if (action == kForeignAidAction) {
    m["type"] = std::any(std::string("foreign_aid"));
  } else if (action >= kCoupOffset && action < kCoupOffset + kCoupCount) {
    m["type"] = std::any(std::string("coup"));
    m["target"] = std::any(action - kCoupOffset);
  } else if (action == kTaxAction) {
    m["type"] = std::any(std::string("tax"));
    m["claimed"] = std::any(std::string("Duke"));
  } else if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) {
    m["type"] = std::any(std::string("assassinate"));
    m["target"] = std::any(action - kAssassinateOffset);
    m["claimed"] = std::any(std::string("Assassin"));
  } else if (action >= kStealOffset && action < kStealOffset + kStealCount) {
    m["type"] = std::any(std::string("steal"));
    m["target"] = std::any(action - kStealOffset);
    m["claimed"] = std::any(std::string("Captain"));
  } else if (action == kExchangeAction) {
    m["type"] = std::any(std::string("exchange"));
    m["claimed"] = std::any(std::string("Ambassador"));
  } else if (action == kChallengeAction) {
    m["type"] = std::any(std::string("challenge"));
  } else if (action == kAllowAction) {
    m["type"] = std::any(std::string("allow"));
  } else if (action == kBlockDukeAction) {
    m["type"] = std::any(std::string("block"));
    m["claimed"] = std::any(std::string("Duke"));
  } else if (action == kBlockContessaAction) {
    m["type"] = std::any(std::string("block"));
    m["claimed"] = std::any(std::string("Contessa"));
  } else if (action == kBlockAmbassadorAction) {
    m["type"] = std::any(std::string("block"));
    m["claimed"] = std::any(std::string("Ambassador"));
  } else if (action == kBlockCaptainAction) {
    m["type"] = std::any(std::string("block"));
    m["claimed"] = std::any(std::string("Captain"));
  } else if (action == kAllowNoBlockAction) {
    m["type"] = std::any(std::string("allow_no_block"));
  } else if (action == kRevealSlot0 || action == kRevealSlot1) {
    m["type"] = std::any(std::string("reveal"));
    m["slot"] = std::any(action == kRevealSlot0 ? 0 : 1);
  } else if (action == kLoseSlot0 || action == kLoseSlot1) {
    m["type"] = std::any(std::string("lose_influence"));
    m["slot"] = std::any(action == kLoseSlot0 ? 0 : 1);
  } else if (action >= kReturnDuke && action <= kReturnContessa) {
    m["type"] = std::any(std::string("return_card"));
    int char_id = action - kReturnDuke;
    m["character"] = std::any(char_id);
    m["character_name"] = std::any(std::string(
        char_id >= 0 && char_id < 5 ? kCharNames[char_id] : ""));
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

// Public-event extractor. The observer sees revealed cards in two cases:
//   1. A player loses influence (revealed flips false -> true).
//   2. A challenged claim is verified: claimer briefly shows the card, the
//      card is shuffled back to deck, and a new one is drawn. `revealed`
//      doesn't flip, but the role is publicly known.
// We emit `card_revealed` in both cases so the belief tracker can null out
// per-player role signals correctly.
//
// We also emit `exchange_complete` when an Ambassador exchange cycle
// finishes, so the tracker can reset all of that player's signals.
template <int NPlayers>
PublicEventTrace extract_coup_events(
    const IGameState& before,
    ActionId action,
    const IGameState& after,
    int /*perspective*/) {
  using namespace board_ai::coup;
  const auto& sb = board_ai::checked_cast<CoupState<NPlayers>>(before);
  const auto& sa = board_ai::checked_cast<CoupState<NPlayers>>(after);
  const auto& db = sb.data;
  const auto& da = sa.data;
  PublicEventTrace out;

  auto emit_reveal = [&](int player, int role) {
    if (player < 0 || player >= NPlayers) return;
    if (role < 0 || role >= kCharacterCount) return;
    AnyMap payload;
    payload["player"] = std::any(player);
    payload["role"] = std::any(role);
    out.post_events.push_back({"card_revealed", std::move(payload)});
  };

  // Case 1: any revealed flag flipped from false to true.
  for (int p = 0; p < NPlayers; ++p) {
    for (int sl = 0; sl < 2; ++sl) {
      if (!db.revealed[p][sl] && da.revealed[p][sl]) {
        emit_reveal(p, static_cast<int>(db.influence[p][sl]));
      }
    }
  }

  // Case 2: successful challenge reveal — stage kResolveChallengeAction /
  // kResolveChallengeCounter with a RevealSlot action, the shown card
  // matched the claim and was reshuffled away. Observable role:
  // before.influence[revealer][slot].
  if (action == kRevealSlot0 || action == kRevealSlot1) {
    int slot = (action == kRevealSlot0) ? 0 : 1;
    int revealer = -1;
    if (db.stage == CoupStage::kResolveChallengeAction) {
      revealer = db.active_player;
    } else if (db.stage == CoupStage::kResolveChallengeCounter) {
      revealer = db.blocker;
    }
    if (revealer >= 0 && revealer < NPlayers &&
        !db.revealed[revealer][slot] && !da.revealed[revealer][slot]) {
      // Success case: role shown publicly, card reshuffled.
      emit_reveal(revealer, static_cast<int>(db.influence[revealer][slot]));
    }
  }

  // Case 3: exchange cycle complete.
  if (db.stage == CoupStage::kExchangeReturn2 &&
      da.stage != CoupStage::kExchangeReturn1 &&
      da.stage != CoupStage::kExchangeReturn2) {
    AnyMap payload;
    payload["player"] = std::any(db.active_player);
    out.post_events.push_back({"exchange_complete", std::move(payload)});
  }

  return out;
}

// initial_observation for AI API: perspective sees their own starting hand.
// Deck / opp hands are hidden and will be filled by randomize_unseen.
template <int NPlayers>
AnyMap extract_coup_initial_observation(const IGameState& state, int perspective) {
  using namespace board_ai::coup;
  const auto& s = board_ai::checked_cast<CoupState<NPlayers>>(state);
  const auto& d = s.data;
  AnyMap out;
  if (perspective >= 0 && perspective < NPlayers) {
    std::vector<int> my_hand;
    for (int sl = 0; sl < 2; ++sl) {
      my_hand.push_back(static_cast<int>(d.influence[perspective][sl]));
    }
    out["my_hand"] = std::any(my_hand);
  }
  out["num_players"] = std::any(NPlayers);
  return out;
}

template <int NPlayers>
void apply_coup_initial_observation(IGameState& state, int perspective, const AnyMap& obs) {
  using namespace board_ai::coup;
  auto& s = board_ai::checked_cast<CoupState<NPlayers>>(state);
  auto& d = s.data;
  if (perspective < 0 || perspective >= NPlayers) return;
  auto it = obs.find("my_hand");
  if (it == obs.end()) return;
  auto my_hand = std::any_cast<std::vector<int>>(it->second);
  for (int sl = 0; sl < 2 && sl < static_cast<int>(my_hand.size()); ++sl) {
    d.influence[perspective][sl] = static_cast<CharId>(my_hand[sl]);
  }
}

// apply_event stub: the AI session advances its own state via do_action_fast
// in the non-deterministic world sampled by randomize_unseen, then applies
// events to overwrite public facts. For Coup, randomize_unseen already
// produces the right public state since revealed cards stay in state, and
// the API needs no further event-side mutation for per-action fidelity.
// We still need to supply a non-empty applier so the framework's event
// pipeline is live — tracker receives events through observe_public_event.
template <int NPlayers>
void apply_coup_event(
    IGameState& /*state*/,
    EventPhase /*phase*/,
    const std::string& /*kind*/,
    const AnyMap& /*payload*/) {
  // No-op: Coup events are advisory signals for the tracker, not state
  // mutations. The ground-truth flow (challenge reveal, lose influence,
  // exchange reshuffle) is already captured by do_action_fast under the
  // sampled world from randomize_unseen.
}

template <int NPlayers>
board_ai::GameBundle make_coup(const std::string& game_id, std::uint64_t seed) {
  using namespace board_ai::coup;
  board_ai::GameBundle b;
  b.game_id = game_id;
  auto s = std::make_unique<CoupState<NPlayers>>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<CoupRules<NPlayers>>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<CoupFeatureEncoder<NPlayers>>();
  b.belief_tracker = std::make_unique<CoupBeliefTracker<NPlayers>>();
  b.state_serializer = serialize_coup<NPlayers>;
  b.action_descriptor = describe_coup;
  b.heuristic_picker = heuristic_random;
  b.public_event_extractor = extract_coup_events<NPlayers>;
  b.public_event_applier = apply_coup_event<NPlayers>;
  b.initial_observation_extractor = extract_coup_initial_observation<NPlayers>;
  b.initial_observation_applier = apply_coup_initial_observation<NPlayers>;
  return b;
}

board_ai::GameRegistrar reg_coup("coup", [](std::uint64_t seed) {
  return make_coup<2>("coup", seed);
});
board_ai::GameRegistrar reg_coup_2p("coup_2p", [](std::uint64_t seed) {
  return make_coup<2>("coup_2p", seed);
});
board_ai::GameRegistrar reg_coup_3p("coup_3p", [](std::uint64_t seed) {
  return make_coup<3>("coup_3p", seed);
});
board_ai::GameRegistrar reg_coup_4p("coup_4p", [](std::uint64_t seed) {
  return make_coup<4>("coup_4p", seed);
});

}  // namespace
