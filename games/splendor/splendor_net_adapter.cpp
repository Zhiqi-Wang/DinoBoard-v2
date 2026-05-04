#include "splendor_net_adapter.h"

#include <algorithm>
#include <vector>

namespace board_ai::splendor {

namespace {

void encode_card(const SplendorCard* card, std::vector<float>* out) {
  if (!card) {
    out->insert(out->end(), 13, 0.0f);
    return;
  }
  out->push_back(1.0f);
  out->push_back(static_cast<float>(card->tier) / 3.0f);
  out->push_back(static_cast<float>(card->points) / 5.0f);
  for (int c = 0; c < kColorCount; ++c) {
    out->push_back(card->bonus == c ? 1.0f : 0.0f);
  }
  for (int c = 0; c < kColorCount; ++c) {
    out->push_back(static_cast<float>(card->cost[static_cast<size_t>(c)]) / 7.0f);
  }
}

void encode_hidden_reserved_placeholder(std::vector<float>* out) {
  out->push_back(1.0f);
  out->insert(out->end(), 12, 0.0f);
}

}  // namespace

// Layout reminder (kFeatureDim = 6 + 15*P + kNobleCount*6 + 12*13 + 39*P + 7):
//   Public (kFeatureDim - 39):
//     - bank (6)
//     - per-player stats in perspective order (15 * P)
//     - nobles (kNobleCount * 6)
//     - tableau (12 * 13)
//     - opp reserved slots (39 * (P-1)): visible → card, hidden → placeholder
//     - metadata (7)
//   Private (39):
//     - perspective's own 3 reserved slots × 13 dims (real cards — visible
//       AND hidden both carry the real card ID since perspective knows them)
template <int NPlayers>
int SplendorFeatureEncoder<NPlayers>::public_feature_dim() const {
  return Cfg::kFeatureDim - Cfg::kPerPlayerReserves;
}

template <int NPlayers>
int SplendorFeatureEncoder<NPlayers>::private_feature_dim() const {
  return Cfg::kPerPlayerReserves;
}

template <int NPlayers>
void SplendorFeatureEncoder<NPlayers>::encode_public(
    const IGameState& state,
    int perspective_player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const SplendorState<NPlayers>*>(&state);
  if (!s || !out || perspective_player < 0 || perspective_player >= Cfg::kPlayers) return;
  const SplendorData<NPlayers>& d = s->persistent.data();
  const auto& cards = splendor_card_pool();
  const auto& nobles = splendor_nobles();

  for (int i = 0; i < kTokenTypes; ++i) {
    const float denom = (i == 5) ? 5.0f : static_cast<float>(Cfg::kGemCount);
    out->push_back(static_cast<float>(d.bank[static_cast<size_t>(i)]) / denom);
  }

  for (int pi = 0; pi < Cfg::kPlayers; ++pi) {
    const int pid = (perspective_player + pi) % Cfg::kPlayers;
    for (int i = 0; i < kTokenTypes; ++i) {
      out->push_back(static_cast<float>(d.player_gems[pid][static_cast<size_t>(i)]) / 10.0f);
    }
    for (int i = 0; i < kColorCount; ++i) {
      out->push_back(static_cast<float>(d.player_bonuses[pid][static_cast<size_t>(i)]) / 7.0f);
    }
    out->push_back(static_cast<float>(d.player_points[pid]) / 20.0f);
    out->push_back(static_cast<float>(d.reserved_size[pid]) / 3.0f);
    out->push_back(static_cast<float>(d.player_cards_count[pid]) / 20.0f);
    out->push_back(static_cast<float>(d.player_nobles_count[pid]) / 3.0f);
  }

  for (int i = 0; i < Cfg::kNobleCount; ++i) {
    if (i < d.nobles_size) {
      const int nid = d.nobles[static_cast<size_t>(i)];
      if (nid >= 0 && nid < static_cast<int>(nobles.size())) {
        for (int c = 0; c < kColorCount; ++c) {
          out->push_back(static_cast<float>(nobles[static_cast<size_t>(nid)][static_cast<size_t>(c)]) / 4.0f);
        }
        out->push_back(1.0f);
      } else {
        out->insert(out->end(), 6, 0.0f);
      }
    } else {
      out->insert(out->end(), 6, 0.0f);
    }
  }

  for (int tier = 0; tier < 3; ++tier) {
    for (int slot = 0; slot < 4; ++slot) {
      const bool exists = slot < d.tableau_size[static_cast<size_t>(tier)];
      if (!exists) {
        out->insert(out->end(), 13, 0.0f);
        continue;
      }
      const int cid = d.tableau[static_cast<size_t>(tier)][static_cast<size_t>(slot)];
      if (cid < 0 || cid >= static_cast<int>(cards.size())) {
        out->insert(out->end(), 13, 0.0f);
      } else {
        encode_card(&cards[static_cast<size_t>(cid)], out);
      }
    }
  }

  // Non-perspective players' reserved slots (visible → card, hidden → placeholder).
  // Perspective's own reserved is encoded in encode_private instead.
  for (int pi = 1; pi < Cfg::kPlayers; ++pi) {
    const int pid = (perspective_player + pi) % Cfg::kPlayers;
    for (int slot = 0; slot < 3; ++slot) {
      const bool exists = slot < d.reserved_size[pid];
      if (!exists) {
        out->insert(out->end(), 13, 0.0f);
        continue;
      }
      const bool visible = d.reserved_visible[pid][static_cast<size_t>(slot)] != 0;
      if (!visible) {
        encode_hidden_reserved_placeholder(out);
        continue;
      }
      const int cid = d.reserved[pid][static_cast<size_t>(slot)];
      if (cid < 0 || cid >= static_cast<int>(cards.size())) {
        out->insert(out->end(), 13, 0.0f);
      } else {
        encode_card(&cards[static_cast<size_t>(cid)], out);
      }
    }
  }

  out->push_back(static_cast<float>(std::min(d.plies, kMaxPlies)) / static_cast<float>(kMaxPlies));
  out->push_back(d.current_player == perspective_player ? 1.0f : 0.0f);
  out->push_back(static_cast<float>(std::max(0, d.pending_returns)) / 5.0f);
  const SplendorTurnStage stage = static_cast<SplendorTurnStage>(d.stage);
  out->push_back(stage == SplendorTurnStage::kNormal ? 1.0f : 0.0f);
  out->push_back(stage == SplendorTurnStage::kReturnTokens ? 1.0f : 0.0f);
  out->push_back(stage == SplendorTurnStage::kChooseNoble ? 1.0f : 0.0f);
  out->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);
}

template <int NPlayers>
void SplendorFeatureEncoder<NPlayers>::encode_private(
    const IGameState& state,
    int player,
    std::vector<float>* out) const {
  const auto* s = dynamic_cast<const SplendorState<NPlayers>*>(&state);
  if (!s || !out || player < 0 || player >= Cfg::kPlayers) return;
  const SplendorData<NPlayers>& d = s->persistent.data();
  const auto& cards = splendor_card_pool();

  // Player's own 3 reserved slots, full card detail regardless of
  // public visibility — `player` is the owner so they always know.
  for (int slot = 0; slot < 3; ++slot) {
    const bool exists = slot < d.reserved_size[player];
    if (!exists) {
      out->insert(out->end(), 13, 0.0f);
      continue;
    }
    const int cid = d.reserved[player][static_cast<size_t>(slot)];
    if (cid < 0 || cid >= static_cast<int>(cards.size())) {
      out->insert(out->end(), 13, 0.0f);
    } else {
      encode_card(&cards[static_cast<size_t>(cid)], out);
    }
  }
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::init(
    int perspective_player, const AnyMap& initial_observation) {
  // Idempotent: if already initialized for this perspective, don't wipe
  // accumulated seen_cards. Callers (selfplay/arena runners) re-init each
  // ply to pick the current acting perspective; we only rebuild seen_cards
  // on the first call.
  if (initialized_ && perspective_player == perspective_player_) return;
  perspective_player_ = perspective_player;
  seen_cards_.clear();
  initialized_ = true;

  // Initial observation carries public tableau (all cards face-up).
  auto it_t = initial_observation.find("tableau");
  if (it_t != initial_observation.end()) {
    const auto& tableau_any = std::any_cast<const std::vector<std::any>&>(it_t->second);
    for (const auto& tier_any : tableau_any) {
      const auto& tier = std::any_cast<const std::vector<int>&>(tier_any);
      for (int cid : tier) {
        if (cid >= 0) seen_cards_.insert(cid);
      }
    }
  }
  // Nobles are public but aren't "cards" for the deck-pool belief.
  // Reserved slots at game start are all empty — nothing to add.
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::observe_public_event(
    int actor,
    ActionId action,
    const std::vector<PublicEvent>& /*pre_events*/,
    const std::vector<PublicEvent>& post_events) {
  // deck_flip post-events carry any new tableau card IDs revealed by
  // drawing from the deck to replace a bought/reserved card. Perspective
  // sees all tableau flips.
  for (const auto& ev : post_events) {
    if (ev.first == "deck_flip") {
      auto cit = ev.second.find("card_id");
      if (cit != ev.second.end()) {
        const int cid = std::any_cast<int>(cit->second);
        if (cid >= 0) seen_cards_.insert(cid);
      }
    } else if (ev.first == "self_reserve_deck") {
      // Emitted only when perspective reserves from deck top; payload
      // carries the now-known card_id for perspective's new reserved slot.
      if (actor == perspective_player_) {
        auto cit = ev.second.find("card_id");
        if (cit != ev.second.end()) {
          const int cid = std::any_cast<int>(cit->second);
          if (cid >= 0) seen_cards_.insert(cid);
        }
      }
    }
  }
  (void)action;
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::randomize_unseen(IGameState& state, std::mt19937& rng) const {
  auto* s = dynamic_cast<SplendorState<NPlayers>*>(&state);
  if (!s) return;

  SplendorData<NPlayers> data = s->persistent.data();
  const auto& cards = splendor_card_pool();
  const int total_cards = static_cast<int>(cards.size());

  std::array<std::vector<int>, 3> unseen_by_tier{};
  for (int cid = 0; cid < total_cards; ++cid) {
    if (seen_cards_.count(cid) == 0) {
      const int tier_idx = cards[static_cast<size_t>(cid)].tier - 1;
      if (tier_idx >= 0 && tier_idx < 3) {
        unseen_by_tier[static_cast<size_t>(tier_idx)].push_back(cid);
      }
    }
  }

  for (int t = 0; t < 3; ++t) {
    std::shuffle(unseen_by_tier[static_cast<size_t>(t)].begin(),
                 unseen_by_tier[static_cast<size_t>(t)].end(), rng);
  }

  std::array<size_t, 3> idx{0, 0, 0};
  for (int p = 0; p < Cfg::kPlayers; ++p) {
    if (p == perspective_player_) continue;
    for (int slot = 0; slot < data.reserved_size[p]; ++slot) {
      if (data.reserved_visible[p][static_cast<size_t>(slot)] == 0) {
        const int cid = data.reserved[p][static_cast<size_t>(slot)];
        if (cid >= 0 && cid < total_cards) {
          const int tier_idx = cards[static_cast<size_t>(cid)].tier - 1;
          if (tier_idx >= 0 && tier_idx < 3) {
            auto& pool = unseen_by_tier[static_cast<size_t>(tier_idx)];
            auto& i = idx[static_cast<size_t>(tier_idx)];
            if (i < pool.size()) {
              data.reserved[p][static_cast<size_t>(slot)] =
                  static_cast<std::int16_t>(pool[i++]);
            }
          }
        }
      }
    }
  }

  for (int t = 0; t < 3; ++t) {
    const auto deck_size = data.decks[static_cast<size_t>(t)].size();
    data.decks[static_cast<size_t>(t)].clear();
    auto& pool = unseen_by_tier[static_cast<size_t>(t)];
    auto& i = idx[static_cast<size_t>(t)];
    for (size_t k = 0; k < deck_size && i < pool.size(); ++k) {
      data.decks[static_cast<size_t>(t)].push_back(
          static_cast<std::int16_t>(pool[i++]));
    }
  }

  data.draw_nonce ^= static_cast<std::uint64_t>(rng());

  auto node = std::make_shared<SplendorPersistentNode<NPlayers>>();
  node->action_from_parent = -1;
  node->materialized = std::make_shared<const SplendorData<NPlayers>>(std::move(data));
  s->persistent = SplendorPersistentState<NPlayers>(node);
  s->undo_stack.clear();
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::reconcile_state(IGameState& state) const {
  // Rebuild per-tier deck content from the tracker's seen_cards.
  //
  // Invariant: after every action, the observer's unseen pool is
  //   pool - seen_cards = (opp hidden reserved from API's current world)
  //                     ∪ (deck content)
  // API's deck should therefore be: (pool_tier - seen_cards_tier) minus
  // the cards that API's current world has placed in opp hidden reserved.
  //
  // This fixes the Splendor-specific drift where per-slot `deck_flip`
  // events conflate slot shifts with real deck draws, causing API's deck
  // size to drift 1 per faceup-reserve / faceup-buy action.
  auto* s = dynamic_cast<SplendorState<NPlayers>*>(&state);
  if (!s) return;
  SplendorData<NPlayers> data = s->persistent.data();
  const auto& pool = splendor_card_pool();
  const int total_cards = static_cast<int>(pool.size());

  // Collect opp hidden reserved card IDs currently in API's world.
  std::unordered_set<int> opp_hidden_reserved;
  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective_player_) continue;
    for (int slot = 0; slot < data.reserved_size[p]; ++slot) {
      if (data.reserved_visible[p][static_cast<size_t>(slot)] == 0) {
        const int cid = data.reserved[p][static_cast<size_t>(slot)];
        if (cid >= 0 && cid < total_cards) {
          opp_hidden_reserved.insert(cid);
        }
      }
    }
  }

  // Rebuild each tier's deck from unseen pool - opp_hidden_reserved.
  std::array<std::vector<int>, 3> new_decks{};
  for (int cid = 0; cid < total_cards; ++cid) {
    if (seen_cards_.count(cid)) continue;
    if (opp_hidden_reserved.count(cid)) continue;
    const int tier_idx = pool[static_cast<size_t>(cid)].tier - 1;
    if (tier_idx >= 0 && tier_idx < 3) {
      new_decks[static_cast<size_t>(tier_idx)].push_back(cid);
    }
  }

  bool changed = false;
  for (int t = 0; t < 3; ++t) {
    auto& deck = data.decks[static_cast<size_t>(t)];
    const auto& nd = new_decks[static_cast<size_t>(t)];
    if (deck.size() != nd.size()) changed = true;
    else {
      // Compare as sets (order doesn't matter — randomize_unseen
      // reshuffles each sim anyway).
      std::unordered_set<int> cur(deck.begin(), deck.end());
      for (int c : nd) if (!cur.count(c)) { changed = true; break; }
    }
    if (changed) {
      deck.clear();
      deck.reserve(nd.size());
      for (int c : nd) deck.push_back(static_cast<std::int16_t>(c));
    }
  }

  if (changed) {
    auto node = std::make_shared<SplendorPersistentNode<NPlayers>>();
    node->action_from_parent = -1;
    node->materialized = std::make_shared<const SplendorData<NPlayers>>(std::move(data));
    s->persistent = SplendorPersistentState<NPlayers>(std::move(node));
    s->undo_stack.clear();
  }
}

template <int NPlayers>
AnyMap SplendorBeliefTracker<NPlayers>::serialize() const {
  AnyMap out;
  out["perspective_player"] = perspective_player_;
  // Canonical form: sorted vector of seen card IDs (unordered_set iteration
  // order varies). Two trackers with the same seen set produce equal output.
  std::vector<int> seen(seen_cards_.begin(), seen_cards_.end());
  std::sort(seen.begin(), seen.end());
  out["seen_cards"] = seen;
  out["initialized"] = initialized_;
  return out;
}

template class SplendorFeatureEncoder<2>;
template class SplendorFeatureEncoder<3>;
template class SplendorFeatureEncoder<4>;
template class SplendorBeliefTracker<2>;
template class SplendorBeliefTracker<3>;
template class SplendorBeliefTracker<4>;

}  // namespace board_ai::splendor
