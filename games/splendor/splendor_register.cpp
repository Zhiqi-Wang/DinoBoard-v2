#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../engine/core/game_registry.h"
#include "../../engine/search/tail_solver.h"
#include "splendor_state.h"
#include "splendor_rules.h"
#include "splendor_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::ActionId;
using board_ai::IGameState;
using board_ai::EventPhase;
using board_ai::PublicEvent;
using board_ai::PublicEventTrace;

static const char* const kColorNames[6] = {"white", "blue", "green", "red", "black", "gold"};

template <int NPlayers>
AnyMap serialize_splendor(const IGameState& state) {
  using Cfg = board_ai::splendor::SplendorConfig<NPlayers>;
  const auto& s = board_ai::checked_cast<board_ai::splendor::SplendorState<NPlayers>>(state);
  const auto& d = s.persistent.data();

  AnyMap m;
  m["current_player"] = std::any(state.current_player());
  m["is_terminal"] = std::any(state.is_terminal());
  m["winner"] = std::any(state.winner());
  m["num_players"] = std::any(NPlayers);
  m["plies"] = std::any(d.plies);
  m["stage"] = std::any(static_cast<int>(d.stage));
  m["pending_returns"] = std::any(d.pending_returns);
  m["final_round_remaining"] = std::any(d.final_round_remaining);

  std::vector<int> scores(NPlayers);
  for (int i = 0; i < NPlayers; ++i) scores[i] = d.scores[i];
  m["scores"] = std::any(scores);

  std::vector<int> bank(board_ai::splendor::kTokenTypes);
  for (int i = 0; i < board_ai::splendor::kTokenTypes; ++i) bank[i] = d.bank[i];
  m["bank"] = std::any(bank);

  std::vector<AnyMap> players;
  for (int p = 0; p < NPlayers; ++p) {
    AnyMap pm;
    std::vector<int> gems(board_ai::splendor::kTokenTypes);
    for (int j = 0; j < board_ai::splendor::kTokenTypes; ++j) gems[j] = d.player_gems[p][j];
    pm["gems"] = std::any(gems);
    std::vector<int> bonuses(board_ai::splendor::kColorCount);
    for (int j = 0; j < board_ai::splendor::kColorCount; ++j) bonuses[j] = d.player_bonuses[p][j];
    pm["bonuses"] = std::any(bonuses);
    pm["points"] = std::any(static_cast<int>(d.player_points[p]));
    pm["cards_count"] = std::any(static_cast<int>(d.player_cards_count[p]));
    pm["nobles_count"] = std::any(static_cast<int>(d.player_nobles_count[p]));

    int rs = d.reserved_size[p];
    std::vector<AnyMap> reserved;
    const auto& pool = board_ai::splendor::splendor_card_pool();
    for (int ri = 0; ri < rs; ++ri) {
      AnyMap rc;
      int card_id = d.reserved[p][ri];
      bool visible = d.reserved_visible[p][ri] != 0;
      rc["visible"] = std::any(visible);
      if (visible && card_id >= 0 && card_id < static_cast<int>(pool.size())) {
        const auto& card = pool[card_id];
        rc["tier"] = std::any(static_cast<int>(card.tier));
        rc["bonus"] = std::any(static_cast<int>(card.bonus));
        rc["points"] = std::any(static_cast<int>(card.points));
        std::vector<int> cost(board_ai::splendor::kColorCount);
        for (int j = 0; j < board_ai::splendor::kColorCount; ++j) cost[j] = card.cost[j];
        rc["cost"] = std::any(cost);
      } else {
        rc["tier"] = std::any(-1);
      }
      reserved.push_back(std::move(rc));
    }
    pm["reserved"] = std::any(reserved);
    players.push_back(std::move(pm));
  }
  m["players"] = std::any(players);

  const auto& pool = board_ai::splendor::splendor_card_pool();
  std::vector<std::vector<AnyMap>> tableau(3);
  for (int tier = 0; tier < 3; ++tier) {
    int ts = d.tableau_size[tier];
    for (int slot = 0; slot < ts; ++slot) {
      int card_id = d.tableau[tier][slot];
      AnyMap cm;
      cm["card_id"] = std::any(card_id);
      if (card_id >= 0 && card_id < static_cast<int>(pool.size())) {
        const auto& card = pool[card_id];
        cm["tier"] = std::any(static_cast<int>(card.tier));
        cm["bonus"] = std::any(static_cast<int>(card.bonus));
        cm["points"] = std::any(static_cast<int>(card.points));
        std::vector<int> cost(board_ai::splendor::kColorCount);
        for (int j = 0; j < board_ai::splendor::kColorCount; ++j) cost[j] = card.cost[j];
        cm["cost"] = std::any(cost);
      }
      tableau[tier].push_back(std::move(cm));
    }
  }
  std::vector<std::any> tableau_any;
  for (auto& t : tableau) tableau_any.push_back(std::any(std::move(t)));
  m["tableau"] = std::any(tableau_any);

  std::vector<int> deck_sizes(3);
  for (int i = 0; i < 3; ++i) deck_sizes[i] = static_cast<int>(d.decks[i].size());
  m["deck_sizes"] = std::any(deck_sizes);

  std::vector<int> all_deck_ids;
  for (int t = 0; t < 3; ++t) {
    for (auto cid : d.decks[t]) all_deck_ids.push_back(static_cast<int>(cid));
  }
  m["_test_all_deck_ids"] = std::any(all_deck_ids);

  std::vector<int> tableau_ids;
  for (int t = 0; t < 3; ++t) {
    for (int slot = 0; slot < d.tableau_size[t]; ++slot) {
      tableau_ids.push_back(static_cast<int>(d.tableau[t][slot]));
    }
  }
  m["_test_tableau_ids"] = std::any(tableau_ids);

  const auto& noble_reqs = board_ai::splendor::splendor_nobles();
  std::vector<AnyMap> nobles;
  for (int ni = 0; ni < d.nobles_size; ++ni) {
    AnyMap nm;
    int noble_id = d.nobles[ni];
    nm["noble_id"] = std::any(noble_id);
    if (noble_id >= 0 && noble_id < static_cast<int>(noble_reqs.size())) {
      std::vector<int> req(board_ai::splendor::kColorCount);
      for (int j = 0; j < board_ai::splendor::kColorCount; ++j) req[j] = noble_reqs[noble_id][j];
      nm["requirements"] = std::any(req);
    }
    nobles.push_back(std::move(nm));
  }
  m["nobles"] = std::any(nobles);

  return m;
}

template <int NPlayers>
AnyMap describe_splendor(ActionId action) {
  using Cfg = board_ai::splendor::SplendorConfig<NPlayers>;
  AnyMap m;
  m["action_id"] = std::any(static_cast<int>(action));

  if (action == Cfg::kPassAction) {
    m["type"] = std::any(std::string("pass"));
  } else if (action >= Cfg::kReturnTokenOffset && action < Cfg::kReturnTokenOffset + Cfg::kReturnTokenCount) {
    int token = action - Cfg::kReturnTokenOffset;
    m["type"] = std::any(std::string("return_token"));
    m["token"] = std::any(token);
    m["token_name"] = std::any(std::string(kColorNames[token]));
  } else if (action >= Cfg::kChooseNobleOffset && action < Cfg::kChooseNobleOffset + Cfg::kChooseNobleCount) {
    m["type"] = std::any(std::string("choose_noble"));
    m["noble_slot"] = std::any(action - Cfg::kChooseNobleOffset);
  } else if (action >= Cfg::kBuyFaceupOffset && action < Cfg::kBuyFaceupOffset + Cfg::kBuyFaceupCount) {
    int idx = action - Cfg::kBuyFaceupOffset;
    m["type"] = std::any(std::string("buy_faceup"));
    m["tier"] = std::any(idx / 4);
    m["slot"] = std::any(idx % 4);
  } else if (action >= Cfg::kReserveFaceupOffset && action < Cfg::kReserveFaceupOffset + Cfg::kReserveFaceupCount) {
    int idx = action - Cfg::kReserveFaceupOffset;
    m["type"] = std::any(std::string("reserve_faceup"));
    m["tier"] = std::any(idx / 4);
    m["slot"] = std::any(idx % 4);
  } else if (action >= Cfg::kReserveDeckOffset && action < Cfg::kReserveDeckOffset + Cfg::kReserveDeckCount) {
    m["type"] = std::any(std::string("reserve_deck"));
    m["tier"] = std::any(action - Cfg::kReserveDeckOffset);
  } else if (action >= Cfg::kBuyReservedOffset && action < Cfg::kBuyReservedOffset + Cfg::kBuyReservedCount) {
    m["type"] = std::any(std::string("buy_reserved"));
    m["slot"] = std::any(action - Cfg::kBuyReservedOffset);
  } else if (action >= Cfg::kTakeThreeOffset && action < Cfg::kTakeThreeOffset + Cfg::kTakeThreeCount) {
    m["type"] = std::any(std::string("take_three"));
    int combo = action - Cfg::kTakeThreeOffset;
    int idx = 0;
    std::vector<int> colors;
    for (int a = 0; a < 5 && static_cast<int>(colors.size()) < 3; ++a)
      for (int b = a + 1; b < 5 && static_cast<int>(colors.size()) < 3; ++b)
        for (int c = b + 1; c < 5; ++c) {
          if (idx == combo) { colors = {a, b, c}; goto done_t3; }
          ++idx;
        }
    done_t3:
    m["colors"] = std::any(colors);
  } else if (action >= Cfg::kTakeTwoDifferentOffset && action < Cfg::kTakeTwoDifferentOffset + Cfg::kTakeTwoDifferentCount) {
    m["type"] = std::any(std::string("take_two_different"));
    int combo = action - Cfg::kTakeTwoDifferentOffset;
    int idx = 0;
    std::vector<int> colors;
    for (int a = 0; a < 5; ++a)
      for (int b = a + 1; b < 5; ++b) {
        if (idx == combo) { colors = {a, b}; goto done_t2d; }
        ++idx;
      }
    done_t2d:
    m["colors"] = std::any(colors);
  } else if (action >= Cfg::kTakeOneOffset && action < Cfg::kTakeOneOffset + Cfg::kTakeOneCount) {
    m["type"] = std::any(std::string("take_one"));
    m["color"] = std::any(action - Cfg::kTakeOneOffset);
  } else if (action >= Cfg::kTakeTwoSameOffset && action < Cfg::kTakeTwoSameOffset + Cfg::kTakeTwoSameCount) {
    m["type"] = std::any(std::string("take_two_same"));
    m["color"] = std::any(action - Cfg::kTakeTwoSameOffset);
  }
  return m;
}

// --- Public-event protocol -------------------------------------------------
//
// Splendor has two hidden-state touchpoints during play:
//   (1) Buying or reserving a face-up card auto-flips a new card from the
//       corresponding tier's deck onto the same tableau slot. The new card's
//       ID is public — every player at the table sees the flip.
//   (2) Reserving from the top of a deck (a "blind reserve"): the drawer
//       sees their new face-down card; other players don't.
//
// Event schema:
//   post-action "deck_flip": {"tier", "slot", "card_id"} — emitted for each
//     tableau slot whose card changed after the action. card_id = -1 means
//     the slot is now empty (deck exhausted).
//   post-action "self_reserve_deck": {"slot", "card_id"} — emitted only when
//     the acting player is the traced perspective AND the action was a
//     reserve-from-deck. Overrides the AI's randomly-drawn card with ground
//     truth's actual card so the belief tracker's seen_cards agrees.
//   (Opponent reserve-from-deck needs no event — AI's random card in the
//    face-down slot is belief-consistent, since AI never learns what
//    opponent reserved blindly.)

namespace splendor_events {

using board_ai::splendor::SplendorConfig;
using board_ai::splendor::SplendorData;
using board_ai::splendor::SplendorState;
using board_ai::splendor::SplendorPersistentNode;
using board_ai::splendor::SplendorPersistentState;

// Safe helper: materialize the state's persistent data as mutable, mutate
// it via a functor, and reseat state.persistent to point at the new data.
template <int NPlayers>
void mutate_persistent(SplendorState<NPlayers>& s,
                       const std::function<void(SplendorData<NPlayers>&)>& fn) {
  SplendorData<NPlayers> data = s.persistent.data();
  fn(data);
  auto node = std::make_shared<SplendorPersistentNode<NPlayers>>();
  node->action_from_parent = -1;
  node->materialized = std::make_shared<const SplendorData<NPlayers>>(std::move(data));
  s.persistent = SplendorPersistentState<NPlayers>(std::move(node));
  s.undo_stack.clear();
}

template <int NPlayers>
AnyMap extract_initial_observation(const IGameState& state, int /*perspective*/) {
  using Cfg = SplendorConfig<NPlayers>;
  const auto& s = board_ai::checked_cast<SplendorState<NPlayers>>(state);
  const auto& d = s.persistent.data();
  AnyMap out;
  std::vector<std::vector<int>> tableau(3);
  for (int t = 0; t < 3; ++t) {
    tableau[t].resize(4, -1);
    for (int slot = 0; slot < d.tableau_size[t]; ++slot) {
      tableau[t][slot] = static_cast<int>(d.tableau[t][slot]);
    }
  }
  std::vector<std::any> tableau_any;
  for (auto& t : tableau) tableau_any.push_back(std::any(std::move(t)));
  out["tableau"] = std::any(tableau_any);

  std::vector<int> nobles;
  for (int i = 0; i < d.nobles_size; ++i) nobles.push_back(static_cast<int>(d.nobles[i]));
  out["nobles"] = std::any(nobles);
  return out;
}

template <int NPlayers>
void apply_initial_observation(IGameState& state, int /*perspective*/, const AnyMap& obs) {
  using Cfg = SplendorConfig<NPlayers>;
  auto& s = board_ai::checked_cast<SplendorState<NPlayers>>(state);
  auto it_t = obs.find("tableau");
  auto it_n = obs.find("nobles");
  if (it_t == obs.end() || it_n == obs.end()) {
    throw std::runtime_error("splendor initial_observation needs 'tableau' and 'nobles'");
  }
  auto tableau_any = std::any_cast<std::vector<std::any>>(it_t->second);
  if (tableau_any.size() != 3) {
    throw std::runtime_error("splendor initial_observation: tableau must have 3 tiers");
  }
  std::array<std::vector<int>, 3> gt_tableau;
  for (int t = 0; t < 3; ++t) {
    gt_tableau[t] = std::any_cast<std::vector<int>>(tableau_any[t]);
  }
  std::vector<int> gt_nobles = std::any_cast<std::vector<int>>(it_n->second);

  mutate_persistent<NPlayers>(s, [&](SplendorData<NPlayers>& d) {
    // Set tableau cards and recompute tableau_size.
    for (int t = 0; t < 3; ++t) {
      int size = 0;
      for (int slot = 0; slot < 4; ++slot) {
        const int cid = (slot < static_cast<int>(gt_tableau[t].size())) ? gt_tableau[t][slot] : -1;
        d.tableau[t][slot] = static_cast<std::int16_t>(cid);
        if (cid >= 0) size = slot + 1;
      }
      d.tableau_size[t] = static_cast<std::int8_t>(size);
    }
    // Set nobles.
    for (int i = 0; i < Cfg::kNobleCount; ++i) {
      d.nobles[i] = (i < static_cast<int>(gt_nobles.size()))
          ? static_cast<std::int16_t>(gt_nobles[i]) : static_cast<std::int16_t>(-1);
    }
    d.nobles_size = static_cast<std::int8_t>(std::min<int>(gt_nobles.size(), Cfg::kNobleCount));

    // Rebuild decks: all cards that aren't on the tableau or in nobles and
    // aren't already dealt/reserved/discarded. At game start, only the
    // tableau has been dealt from decks. The persistent tree cache is now
    // stale — mutate_persistent resets that via reseating.
    std::unordered_set<int> on_tableau;
    for (int t = 0; t < 3; ++t) {
      for (int slot = 0; slot < d.tableau_size[t]; ++slot) {
        if (d.tableau[t][slot] >= 0) on_tableau.insert(d.tableau[t][slot]);
      }
    }
    const auto& pool = board_ai::splendor::splendor_card_pool();
    for (int t = 0; t < 3; ++t) d.decks[t].clear();
    for (int cid = 0; cid < static_cast<int>(pool.size()); ++cid) {
      if (on_tableau.count(cid)) continue;
      const int tier = pool[cid].tier - 1;
      if (tier >= 0 && tier < 3) {
        d.decks[tier].push_back(static_cast<std::int16_t>(cid));
      }
    }
  });
}

template <int NPlayers>
PublicEventTrace extract_events(
    const IGameState& before,
    ActionId action,
    const IGameState& after,
    int perspective) {
  using Cfg = SplendorConfig<NPlayers>;
  const auto& sb = board_ai::checked_cast<SplendorState<NPlayers>>(before);
  const auto& sa = board_ai::checked_cast<SplendorState<NPlayers>>(after);
  const auto& db = sb.persistent.data();
  const auto& da = sa.persistent.data();
  PublicEventTrace out;

  // deck_flip: any tableau slot whose card changed is a public flip.
  for (int t = 0; t < 3; ++t) {
    const int max_slot = std::max<int>(db.tableau_size[t], da.tableau_size[t]);
    for (int slot = 0; slot < max_slot; ++slot) {
      const int before_id = (slot < db.tableau_size[t]) ? static_cast<int>(db.tableau[t][slot]) : -1;
      const int after_id = (slot < da.tableau_size[t]) ? static_cast<int>(da.tableau[t][slot]) : -1;
      if (before_id != after_id) {
        AnyMap payload;
        payload["tier"] = std::any(t);
        payload["slot"] = std::any(slot);
        payload["card_id"] = std::any(after_id);
        out.post_events.emplace_back("deck_flip", std::move(payload));
      }
    }
  }

  // self_reserve_deck: emitted only when perspective reserved from deck top.
  const int actor = db.current_player;
  const bool is_reserve_deck = action >= Cfg::kReserveDeckOffset &&
      action < Cfg::kReserveDeckOffset + Cfg::kReserveDeckCount;
  if (is_reserve_deck && actor == perspective) {
    // Find the new reserved slot (one whose card changed from -1 to a valid ID).
    for (int slot = 0; slot < 3; ++slot) {
      const int before_id = (slot < db.reserved_size[actor]) ? static_cast<int>(db.reserved[actor][slot]) : -1;
      const int after_id = (slot < da.reserved_size[actor]) ? static_cast<int>(da.reserved[actor][slot]) : -1;
      if (before_id != after_id) {
        AnyMap payload;
        payload["player"] = std::any(actor);
        payload["slot"] = std::any(slot);
        payload["card_id"] = std::any(after_id);
        out.post_events.emplace_back("self_reserve_deck", std::move(payload));
      }
    }
  }
  return out;
}

template <int NPlayers>
void apply_event(IGameState& state, EventPhase phase,
                 const std::string& kind, const AnyMap& payload) {
  if (phase != EventPhase::kPostAction) {
    throw std::runtime_error(
        "splendor: unexpected pre-action event '" + kind + "'");
  }
  auto& s = board_ai::checked_cast<SplendorState<NPlayers>>(state);
  if (kind == "deck_flip") {
    const int tier = std::any_cast<int>(payload.at("tier"));
    const int slot = std::any_cast<int>(payload.at("slot"));
    const int cid = std::any_cast<int>(payload.at("card_id"));
    mutate_persistent<NPlayers>(s, [&](SplendorData<NPlayers>& d) {
      if (cid >= 0) {
        d.tableau[tier][slot] = static_cast<std::int16_t>(cid);
        if (slot + 1 > d.tableau_size[tier]) d.tableau_size[tier] = static_cast<std::int8_t>(slot + 1);
        // Remove cid from AI's deck if present (AI might have had this
        // card scheduled for a different slot).
        auto& deck = d.decks[tier];
        deck.erase(std::remove(deck.begin(), deck.end(),
                               static_cast<std::int16_t>(cid)), deck.end());
      } else {
        d.tableau[tier][slot] = -1;
        // Slot went empty; shrink size if we popped off the end.
        int new_size = 0;
        for (int s2 = 0; s2 < 4; ++s2) {
          if (d.tableau[tier][s2] >= 0) new_size = s2 + 1;
        }
        d.tableau_size[tier] = static_cast<std::int8_t>(new_size);
      }
    });
  } else if (kind == "self_reserve_deck") {
    const int target_player = std::any_cast<int>(payload.at("player"));
    const int slot = std::any_cast<int>(payload.at("slot"));
    const int cid = std::any_cast<int>(payload.at("card_id"));
    if (target_player < 0 || target_player >= NPlayers) {
      throw std::runtime_error("splendor self_reserve_deck: bad player");
    }
    mutate_persistent<NPlayers>(s, [&](SplendorData<NPlayers>& d) {
      if (slot < 0 || slot >= 3 || slot >= d.reserved_size[target_player]) {
        throw std::runtime_error(
            "splendor self_reserve_deck: slot out of range for player " +
            std::to_string(target_player));
      }
      auto& new_card_ref = d.reserved[target_player][slot];
      const int old_cid = new_card_ref;
      new_card_ref = static_cast<std::int16_t>(cid);

      const auto& pool = board_ai::splendor::splendor_card_pool();
      const int tier = (cid >= 0 && cid < static_cast<int>(pool.size()))
          ? pool[cid].tier - 1 : -1;
      if (tier >= 0 && tier < 3) {
        auto& deck = d.decks[tier];
        // Remove cid from deck (it's now in player's hand).
        deck.erase(std::remove(deck.begin(), deck.end(),
                               static_cast<std::int16_t>(cid)), deck.end());
        // Push old_cid back into deck if it was distinct and valid.
        if (old_cid >= 0 && old_cid != cid &&
            old_cid < static_cast<int>(pool.size())) {
          const int old_tier = pool[old_cid].tier - 1;
          if (old_tier >= 0 && old_tier < 3) {
            d.decks[old_tier].push_back(static_cast<std::int16_t>(old_cid));
          }
        }
      }
    });
  } else {
    throw std::runtime_error("splendor: unknown event kind '" + kind + "'");
  }
}

}  // namespace splendor_events

template <int NPlayers>
board_ai::GameBundle make_splendor(const std::string& game_id, std::uint64_t seed) {
  using Cfg = board_ai::splendor::SplendorConfig<NPlayers>;
  board_ai::GameBundle b;
  b.game_id = game_id;
  auto s = std::make_unique<board_ai::splendor::SplendorState<NPlayers>>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<board_ai::splendor::SplendorRules<NPlayers>>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<board_ai::splendor::SplendorFeatureEncoder<NPlayers>>();
  b.belief_tracker = std::make_unique<board_ai::splendor::SplendorBeliefTracker<NPlayers>>();
  b.stochastic_detector = board_ai::default_stochastic_detector;
  b.state_serializer = serialize_splendor<NPlayers>;
  b.action_descriptor = describe_splendor<NPlayers>;
  b.public_event_extractor = splendor_events::extract_events<NPlayers>;
  b.public_event_applier = splendor_events::apply_event<NPlayers>;
  b.initial_observation_extractor = splendor_events::extract_initial_observation<NPlayers>;
  b.initial_observation_applier = splendor_events::apply_initial_observation<NPlayers>;

  b.tail_solver = std::make_unique<board_ai::search::AlphaBetaTailSolver>();
  // SplendorRules::do_action_deterministic sets forced_draw_override = -2
  // so replenishing the visible tier skips drawing from the hidden deck.
  // Tail solver never consumes hidden chance outcomes → safe.
  b.stochastic_tail_solve_safe = true;

  b.tail_solve_trigger = [](const board_ai::IGameState& state, int ply) -> bool {
    if (ply < 40) return false;
    const auto& s = board_ai::checked_cast<board_ai::splendor::SplendorState<NPlayers>>(state);
    const auto& d = s.persistent.data();
    for (int p = 0; p < NPlayers; ++p) {
      if (d.player_points[static_cast<size_t>(p)] >= 12) return true;
    }
    return false;
  };

  b.episode_stats_extractor = [](
      const board_ai::IGameState& final_state,
      const std::vector<board_ai::SelfplaySampleView>& samples)
      -> std::map<std::string, double> {
    int main_actions = 0;
    for (const auto& s : samples) {
      if (s.action_id < Cfg::kChooseNobleOffset || s.action_id == Cfg::kPassAction) {
        ++main_actions;
      }
    }
    double turns = static_cast<double>(main_actions) / NPlayers;
    return {{"turns", turns}};
  };

  return b;
}

board_ai::GameRegistrar reg_splendor("splendor", [](std::uint64_t seed) {
  return make_splendor<2>("splendor", seed);
});
board_ai::GameRegistrar reg_splendor_2p("splendor_2p", [](std::uint64_t seed) {
  return make_splendor<2>("splendor_2p", seed);
});
board_ai::GameRegistrar reg_splendor_3p("splendor_3p", [](std::uint64_t seed) {
  return make_splendor<3>("splendor_3p", seed);
});
board_ai::GameRegistrar reg_splendor_4p("splendor_4p", [](std::uint64_t seed) {
  return make_splendor<4>("splendor_4p", seed);
});

}  // namespace
