#include "loveletter_net_adapter.h"

#include <algorithm>

namespace board_ai::loveletter {

template <int NPlayers>
bool LoveLetterFeatureEncoder<NPlayers>::encode(
    const IGameState& state,
    int perspective_player,
    const std::vector<ActionId>& legal_actions,
    std::vector<float>* features,
    std::vector<float>* legal_mask) const {
  const auto* s = dynamic_cast<const LoveLetterState<NPlayers>*>(&state);
  if (!s || !features || !legal_mask ||
      perspective_player < 0 || perspective_player >= NPlayers) return false;
  const auto& d = s->data;

  features->clear();
  features->reserve(static_cast<size_t>(Cfg::kFeatureDim));

  for (int pi = 0; pi < NPlayers; ++pi) {
    const int pid = (perspective_player + pi) % NPlayers;
    const bool is_self = (pid == perspective_player);

    features->push_back(d.alive[pid] ? 1.0f : 0.0f);
    features->push_back(d.protected_flags[pid] ? 1.0f : 0.0f);
    features->push_back(d.current_player == pid ? 1.0f : 0.0f);
    features->push_back(d.hand_exposed[pid] ? 1.0f : 0.0f);

    std::int8_t visible_hand = 0;
    if (is_self) {
      visible_hand = d.hand[pid];
    } else if (tracker_ && tracker_->known_hand(pid) > 0) {
      visible_hand = tracker_->known_hand(pid);
    }
    for (int c = 1; c <= kCardTypes; ++c) {
      features->push_back(visible_hand == c ? 1.0f : 0.0f);
    }

    for (int c = 1; c <= kCardTypes; ++c) {
      if (is_self && d.current_player == pid && d.drawn_card == c) {
        features->push_back(1.0f);
      } else {
        features->push_back(0.0f);
      }
    }

    for (int c = 1; c <= kCardTypes; ++c) {
      int count = 0;
      for (auto card : d.discard_piles[static_cast<size_t>(pid)]) {
        if (card == c) ++count;
      }
      features->push_back(static_cast<float>(count) / static_cast<float>(kCardCounts[static_cast<size_t>(c)]));
    }

    features->push_back(static_cast<float>(d.discard_piles[static_cast<size_t>(pid)].size()) / 8.0f);
  }

  features->push_back(static_cast<float>(d.deck.size()) / 16.0f);
  features->push_back(static_cast<float>(d.ply) / 20.0f);
  features->push_back(d.first_player == perspective_player ? 1.0f : 0.0f);

  int alive_count = 0;
  for (int p = 0; p < NPlayers; ++p) {
    if (d.alive[p]) ++alive_count;
  }
  features->push_back(static_cast<float>(alive_count) / static_cast<float>(NPlayers));

  for (int c = 1; c <= kCardTypes; ++c) {
    int count = 0;
    for (auto card : d.face_up_removed) {
      if (card == c) ++count;
    }
    features->push_back(static_cast<float>(count) / static_cast<float>(kCardCounts[static_cast<size_t>(c)]));
  }

  fill_legal_mask(kActionSpace, legal_actions, legal_mask);
  return static_cast<int>(features->size()) == Cfg::kFeatureDim;
}

template <int NPlayers>
void LoveLetterBeliefTracker<NPlayers>::init(
    const IGameState& /*state*/, int perspective_player) {
  if (perspective_player != perspective_player_) {
    known_hand_.fill(0);
  }
  perspective_player_ = perspective_player;
}

template <int NPlayers>
void LoveLetterBeliefTracker<NPlayers>::observe_action(
    const IGameState& state_before,
    ActionId action,
    const IGameState& state_after) {
  const auto* sb = dynamic_cast<const LoveLetterState<NPlayers>*>(&state_before);
  const auto* sa = dynamic_cast<const LoveLetterState<NPlayers>*>(&state_after);
  if (!sb || !sa) return;
  const auto& db = sb->data;
  const auto& da = sa->data;
  const int actor = db.current_player;

  std::int8_t card = 0;
  int target = -1;
  if (action >= kGuardOffset && action < kGuardOffset + kGuardCount) {
    card = kGuard;
    int idx = action - kGuardOffset;
    target = idx / 7;
  } else if (action >= kPriestOffset && action < kPriestOffset + kPriestCount) {
    card = kPriest;
    target = action - kPriestOffset;
  } else if (action >= kBaronOffset && action < kBaronOffset + kBaronCount) {
    card = kBaron;
    target = action - kBaronOffset;
  } else if (action == kHandmaidAction) {
    card = kHandmaid;
  } else if (action >= kPrinceOffset && action < kPrinceOffset + kPrinceCount) {
    card = kPrince;
    target = action - kPrinceOffset;
  } else if (action >= kKingOffset && action < kKingOffset + kKingCount) {
    card = kKing;
    target = action - kKingOffset;
  } else if (action == kCountessAction) {
    card = kCountess;
  } else if (action == kPrincessAction) {
    card = kPrincess;
  }

  const bool i_am_actor = (actor == perspective_player_);
  const bool i_am_target = (target == perspective_player_);

  if (!i_am_actor && known_hand_[actor] != 0) {
    if (card == known_hand_[actor]) {
      known_hand_[actor] = 0;
    }
  }

  switch (card) {
    case kPriest:
      if (i_am_actor && target >= 0 && target < NPlayers && da.alive[target]) {
        known_hand_[target] = da.hand[target];
      }
      break;

    case kBaron:
      if (target >= 0 && target < NPlayers) {
        if (i_am_actor && da.alive[target]) {
          known_hand_[target] = da.hand[target];
        }
        if (i_am_target && da.alive[actor]) {
          known_hand_[actor] = da.hand[actor];
        }
      }
      break;

    case kKing:
      if (target >= 0 && target < NPlayers) {
        if (i_am_actor) {
          std::int8_t remaining = (db.hand[perspective_player_] == kKing)
              ? db.drawn_card : db.hand[perspective_player_];
          known_hand_[target] = remaining;
        } else if (i_am_target) {
          known_hand_[actor] = db.hand[perspective_player_];
        } else {
          std::int8_t ka = known_hand_[actor];
          std::int8_t kt = known_hand_[target];
          if (ka != 0 && ka != kKing) {
            known_hand_[target] = ka;
          } else {
            known_hand_[target] = 0;
          }
          known_hand_[actor] = kt;
        }
      }
      break;

    case kPrince:
      if (target >= 0 && target < NPlayers && target != perspective_player_) {
        known_hand_[target] = 0;
      }
      break;

    default:
      break;
  }

  for (int p = 0; p < NPlayers; ++p) {
    if (!da.alive[p]) known_hand_[p] = 0;
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

  if (perspective_player_ >= 0 && perspective_player_ < NPlayers &&
      d.alive[perspective_player_]) {
    consume(d.hand[perspective_player_]);
    if (d.current_player == perspective_player_ && d.drawn_card != 0) {
      consume(d.drawn_card);
    }
  }

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
std::map<std::string, std::any> LoveLetterBeliefTracker<NPlayers>::serialize() const {
  std::map<std::string, std::any> out;
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
