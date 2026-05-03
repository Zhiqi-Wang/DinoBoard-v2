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

template <int NPlayers>
bool SplendorFeatureEncoder<NPlayers>::encode(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* features,
    std::vector<float>* legal_mask) const {
  const auto* s = dynamic_cast<const SplendorState<NPlayers>*>(&state);
  if (!s || !features || !legal_mask || perspective_player < 0 || perspective_player >= Cfg::kPlayers) return false;
  const SplendorData<NPlayers>& d = s->persistent.data();
  const auto& cards = splendor_card_pool();
  const auto& nobles = splendor_nobles();

  features->clear();
  features->reserve(Cfg::kFeatureDim);

  for (int i = 0; i < kTokenTypes; ++i) {
    const float denom = (i == 5) ? 5.0f : static_cast<float>(Cfg::kGemCount);
    features->push_back(static_cast<float>(d.bank[static_cast<size_t>(i)]) / denom);
  }

  for (int pi = 0; pi < Cfg::kPlayers; ++pi) {
    const int pid = (perspective_player + pi) % Cfg::kPlayers;
    for (int i = 0; i < kTokenTypes; ++i) {
      features->push_back(static_cast<float>(d.player_gems[pid][static_cast<size_t>(i)]) / 10.0f);
    }
    for (int i = 0; i < kColorCount; ++i) {
      features->push_back(static_cast<float>(d.player_bonuses[pid][static_cast<size_t>(i)]) / 7.0f);
    }
    features->push_back(static_cast<float>(d.player_points[pid]) / 20.0f);
    features->push_back(static_cast<float>(d.reserved_size[pid]) / 3.0f);
    features->push_back(static_cast<float>(d.player_cards_count[pid]) / 20.0f);
    features->push_back(static_cast<float>(d.player_nobles_count[pid]) / 3.0f);
  }

  for (int i = 0; i < Cfg::kNobleCount; ++i) {
    if (i < d.nobles_size) {
      const int nid = d.nobles[static_cast<size_t>(i)];
      if (nid >= 0 && nid < static_cast<int>(nobles.size())) {
        for (int c = 0; c < kColorCount; ++c) {
          features->push_back(static_cast<float>(nobles[static_cast<size_t>(nid)][static_cast<size_t>(c)]) / 4.0f);
        }
        features->push_back(1.0f);
      } else {
        features->insert(features->end(), 6, 0.0f);
      }
    } else {
      features->insert(features->end(), 6, 0.0f);
    }
  }

  for (int tier = 0; tier < 3; ++tier) {
    for (int slot = 0; slot < 4; ++slot) {
      const bool exists = slot < d.tableau_size[static_cast<size_t>(tier)];
      if (!exists) {
        features->insert(features->end(), 13, 0.0f);
        continue;
      }
      const int cid = d.tableau[static_cast<size_t>(tier)][static_cast<size_t>(slot)];
      if (cid < 0 || cid >= static_cast<int>(cards.size())) {
        features->insert(features->end(), 13, 0.0f);
      } else {
        encode_card(&cards[static_cast<size_t>(cid)], features);
      }
    }
  }

  for (int pi = 0; pi < Cfg::kPlayers; ++pi) {
    const int pid = (perspective_player + pi) % Cfg::kPlayers;
    const bool is_self = (pid == perspective_player);
    for (int slot = 0; slot < 3; ++slot) {
      const bool exists = slot < d.reserved_size[pid];
      if (!exists) {
        features->insert(features->end(), 13, 0.0f);
        continue;
      }
      if (!is_self) {
        const bool visible = d.reserved_visible[pid][static_cast<size_t>(slot)] != 0;
        if (!visible) {
          encode_hidden_reserved_placeholder(features);
          continue;
        }
      }
      const int cid = d.reserved[pid][static_cast<size_t>(slot)];
      if (cid < 0 || cid >= static_cast<int>(cards.size())) {
        features->insert(features->end(), 13, 0.0f);
      } else {
        encode_card(&cards[static_cast<size_t>(cid)], features);
      }
    }
  }

  features->push_back(static_cast<float>(std::min(d.plies, kMaxPlies)) / static_cast<float>(kMaxPlies));
  features->push_back(d.current_player == perspective_player ? 1.0f : 0.0f);
  features->push_back(static_cast<float>(std::max(0, d.pending_returns)) / 5.0f);
  const SplendorTurnStage stage = static_cast<SplendorTurnStage>(d.stage);
  features->push_back(stage == SplendorTurnStage::kNormal ? 1.0f : 0.0f);
  features->push_back(stage == SplendorTurnStage::kReturnTokens ? 1.0f : 0.0f);
  features->push_back(stage == SplendorTurnStage::kChooseNoble ? 1.0f : 0.0f);
  features->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);

  if (features->size() < static_cast<size_t>(Cfg::kFeatureDim)) {
    features->insert(features->end(), static_cast<size_t>(Cfg::kFeatureDim) - features->size(), 0.0f);
  } else if (features->size() > static_cast<size_t>(Cfg::kFeatureDim)) {
    features->resize(static_cast<size_t>(Cfg::kFeatureDim));
  }

  fill_legal_mask(Cfg::kActionSpace, legal_actions, legal_mask);
  return static_cast<int>(features->size()) == Cfg::kFeatureDim;
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::init(const IGameState& state, int perspective_player) {
  perspective_player_ = perspective_player;
  if (initialized_) return;

  const auto* s = dynamic_cast<const SplendorState<NPlayers>*>(&state);
  if (!s) return;
  const auto& data = s->persistent.data();

  seen_cards_.clear();
  for (int t = 0; t < 3; ++t) {
    for (int slot = 0; slot < data.tableau_size[t]; ++slot) {
      const int cid = data.tableau[static_cast<size_t>(t)][static_cast<size_t>(slot)];
      if (cid >= 0) seen_cards_.insert(cid);
    }
  }
  for (int p = 0; p < Cfg::kPlayers; ++p) {
    for (int slot = 0; slot < data.reserved_size[p]; ++slot) {
      if (p == perspective_player_ || data.reserved_visible[p][static_cast<size_t>(slot)] != 0) {
        const int cid = data.reserved[p][static_cast<size_t>(slot)];
        if (cid >= 0) seen_cards_.insert(cid);
      }
    }
  }
  initialized_ = true;
}

template <int NPlayers>
void SplendorBeliefTracker<NPlayers>::observe_action(
    const IGameState& state_before,
    ActionId action,
    const IGameState& state_after) {
  const auto* s_after = dynamic_cast<const SplendorState<NPlayers>*>(&state_after);
  if (!s_after) return;
  const auto& d_after = s_after->persistent.data();

  const bool is_buy_faceup = action >= Cfg::kBuyFaceupOffset &&
      action < Cfg::kBuyFaceupOffset + Cfg::kBuyFaceupCount;
  const bool is_reserve_faceup = action >= Cfg::kReserveFaceupOffset &&
      action < Cfg::kReserveFaceupOffset + Cfg::kReserveFaceupCount;

  if (is_buy_faceup || is_reserve_faceup) {
    for (int t = 0; t < 3; ++t) {
      for (int slot = 0; slot < d_after.tableau_size[t]; ++slot) {
        const int cid = d_after.tableau[static_cast<size_t>(t)][static_cast<size_t>(slot)];
        if (cid >= 0) seen_cards_.insert(cid);
      }
    }
  }

  const bool is_reserve_deck = action >= Cfg::kReserveDeckOffset &&
      action < Cfg::kReserveDeckOffset + Cfg::kReserveDeckCount;
  if (is_reserve_deck) {
    const int actor = state_before.current_player();
    if (actor == perspective_player_) {
      for (int slot = 0; slot < d_after.reserved_size[perspective_player_]; ++slot) {
        const int cid = d_after.reserved[perspective_player_][static_cast<size_t>(slot)];
        if (cid >= 0) seen_cards_.insert(cid);
      }
    }
  }
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
std::map<std::string, std::any> SplendorBeliefTracker<NPlayers>::serialize() const {
  std::map<std::string, std::any> out;
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
