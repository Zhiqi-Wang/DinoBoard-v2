#include "coup_net_adapter.h"

#include <algorithm>

namespace board_ai::coup {

namespace {

template <int NPlayers>
int influence_count(const CoupData<NPlayers>& d, int p) {
  int n = 0;
  if (!d.revealed[p][0]) ++n;
  if (!d.revealed[p][1]) ++n;
  return n;
}

int action_type_index(ActionId action) {
  if (action == kIncomeAction) return 0;
  if (action == kForeignAidAction) return 1;
  if (action >= kCoupOffset && action < kCoupOffset + kCoupCount) return 2;
  if (action == kTaxAction) return 3;
  if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) return 4;
  if (action >= kStealOffset && action < kStealOffset + kStealCount) return 5;
  if (action == kExchangeAction) return 6;
  return -1;
}

}  // namespace

template <int NPlayers>
bool CoupFeatureEncoder<NPlayers>::encode(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* features,
    std::vector<float>* legal_mask) const {
  const auto* s = dynamic_cast<const CoupState<NPlayers>*>(&state);
  if (!s || !features || !legal_mask ||
      perspective_player < 0 || perspective_player >= NPlayers) return false;
  const auto& d = s->data;

  features->clear();
  features->reserve(static_cast<size_t>(Cfg::kFeatureDim));

  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (perspective_player + pi) % NPlayers;
    const bool is_self = (pid == perspective_player);

    features->push_back(d.alive[pid] ? 1.0f : 0.0f);
    features->push_back(static_cast<float>(d.coins[pid]) / 12.0f);

    int inf = influence_count(d, pid);
    features->push_back(inf >= 1 ? 1.0f : 0.0f);
    features->push_back(inf >= 2 ? 1.0f : 0.0f);

    for (int c = 0; c < kCharacterCount; ++c) {
      int count = 0;
      for (int sl = 0; sl < 2; ++sl) {
        if (d.revealed[pid][sl] && d.influence[pid][sl] == c) ++count;
      }
      features->push_back(static_cast<float>(count));
    }

    for (int c = 0; c < kCharacterCount; ++c) {
      if (is_self) {
        int count = 0;
        for (int sl = 0; sl < 2; ++sl) {
          if (!d.revealed[pid][sl] && d.influence[pid][sl] == c) ++count;
        }
        features->push_back(static_cast<float>(count));
      } else {
        features->push_back(0.0f);
      }
    }

    features->push_back(d.active_player == pid ? 1.0f : 0.0f);
    features->push_back(d.action_target == pid ? 1.0f : 0.0f);
    features->push_back(d.blocker == pid ? 1.0f : 0.0f);
    features->push_back(d.challenger == pid ? 1.0f : 0.0f);
  }

  constexpr int kStageCount = 11;
  int stage_idx = static_cast<int>(d.stage);
  for (int i = 0; i < kStageCount; ++i) {
    features->push_back(i == stage_idx ? 1.0f : 0.0f);
  }

  int action_type = action_type_index(d.declared_action);
  constexpr int kActionTypeCount = 7;
  for (int i = 0; i < kActionTypeCount; ++i) {
    features->push_back(i == action_type ? 1.0f : 0.0f);
  }

  constexpr float kMaxPlies = 200.0f;
  features->push_back(static_cast<float>(d.ply) / kMaxPlies);
  features->push_back(static_cast<float>(d.court_deck.size()) / 15.0f);
  features->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);

  fill_legal_mask(kActionSpace, legal_actions, legal_mask);
  return static_cast<int>(features->size()) == Cfg::kFeatureDim;
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::init(
    const IGameState& /*state*/, int perspective_player) {
  perspective_player_ = perspective_player;
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::observe_action(
    const IGameState& /*state_before*/,
    ActionId /*action*/,
    const IGameState& /*state_after*/) {
}

template <int NPlayers>
void CoupBeliefTracker<NPlayers>::randomize_unseen(
    IGameState& state, std::mt19937& rng) const {
  auto* s = dynamic_cast<CoupState<NPlayers>*>(&state);
  if (!s) return;
  auto& d = s->data;

  std::array<int, kCharacterCount> remaining{};
  for (int c = 0; c < kCharacterCount; ++c) {
    remaining[static_cast<size_t>(c)] = kCardsPerCharacter;
  }

  auto consume = [&](CharId card) {
    if (card >= 0 && card < kCharacterCount) {
      remaining[static_cast<size_t>(card)]--;
    }
  };

  for (int p = 0; p < NPlayers; ++p) {
    for (int sl = 0; sl < 2; ++sl) {
      if (d.revealed[p][sl]) {
        consume(d.influence[p][sl]);
      }
    }
  }

  if (perspective_player_ >= 0 && perspective_player_ < NPlayers &&
      d.alive[perspective_player_]) {
    for (int sl = 0; sl < 2; ++sl) {
      if (!d.revealed[perspective_player_][sl]) {
        consume(d.influence[perspective_player_][sl]);
      }
    }
  }

  for (int i = 0; i < 2; ++i) {
    if (d.exchange_drawn[i] >= 0 && d.current_player == perspective_player_) {
      consume(d.exchange_drawn[i]);
    }
  }

  std::vector<CharId> unseen;
  for (int c = 0; c < kCharacterCount; ++c) {
    for (int i = 0; i < remaining[static_cast<size_t>(c)]; ++i) {
      unseen.push_back(static_cast<CharId>(c));
    }
  }
  std::shuffle(unseen.begin(), unseen.end(), rng);

  size_t idx = 0;

  for (int p = 0; p < NPlayers; ++p) {
    if (p == perspective_player_) continue;
    for (int sl = 0; sl < 2; ++sl) {
      if (!d.revealed[p][sl] && idx < unseen.size()) {
        d.influence[p][sl] = unseen[idx++];
      }
    }
  }

  for (int i = 0; i < 2; ++i) {
    if (d.exchange_drawn[i] >= 0 && d.current_player != perspective_player_) {
      if (idx < unseen.size()) {
        d.exchange_drawn[i] = unseen[idx++];
      }
    }
  }

  d.court_deck.clear();
  while (idx < unseen.size()) {
    d.court_deck.push_back(unseen[idx++]);
  }

  d.draw_nonce ^= static_cast<std::uint64_t>(rng());
}

template class CoupFeatureEncoder<2>;
template class CoupFeatureEncoder<3>;
template class CoupFeatureEncoder<4>;
template class CoupBeliefTracker<2>;
template class CoupBeliefTracker<3>;
template class CoupBeliefTracker<4>;

}  // namespace board_ai::coup
