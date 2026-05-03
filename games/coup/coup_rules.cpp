#include "coup_rules.h"

#include <algorithm>

namespace board_ai::coup {

namespace {

CharId draw_from_deck(std::vector<CharId>& deck, std::uint64_t& nonce) {
  if (deck.empty()) return -1;
  const std::uint64_t r = splitmix64(nonce);
  const size_t idx = static_cast<size_t>(r % static_cast<std::uint64_t>(deck.size()));
  const CharId card = deck[idx];
  if (idx + 1 < deck.size()) {
    deck[idx] = deck.back();
  }
  deck.pop_back();
  return card;
}

template <int NPlayers>
int influence_count(const CoupData<NPlayers>& d, int p) {
  int n = 0;
  if (!d.revealed[p][0]) ++n;
  if (!d.revealed[p][1]) ++n;
  return n;
}

template <int NPlayers>
bool has_character(const CoupData<NPlayers>& d, int p, CharId c) {
  for (int s = 0; s < 2; ++s) {
    if (!d.revealed[p][s] && d.influence[p][s] == c) return true;
  }
  return false;
}

template <int NPlayers>
void eliminate_check(CoupData<NPlayers>& d, int p) {
  if (influence_count(d, p) == 0) {
    d.alive[p] = false;
  }
}

template <int NPlayers>
void lose_influence_at_slot(CoupData<NPlayers>& d, int p, int slot) {
  d.revealed[p][slot] = true;
  eliminate_check(d, p);
}

template <int NPlayers>
int count_alive(const CoupData<NPlayers>& d) {
  int n = 0;
  for (int p = 0; p < NPlayers; ++p) {
    if (d.alive[p]) ++n;
  }
  return n;
}

template <int NPlayers>
int sole_survivor(const CoupData<NPlayers>& d) {
  int survivor = -1;
  for (int p = 0; p < NPlayers; ++p) {
    if (d.alive[p]) {
      if (survivor >= 0) return -1;
      survivor = p;
    }
  }
  return survivor;
}

template <int NPlayers>
bool check_game_end(CoupData<NPlayers>& d) {
  int surv = sole_survivor(d);
  if (surv >= 0) {
    d.terminal = true;
    d.winner = surv;
    return true;
  }
  return false;
}

template <int NPlayers>
int next_alive_player(const CoupData<NPlayers>& d, int from) {
  for (int i = 1; i <= NPlayers; ++i) {
    int p = (from + i) % NPlayers;
    if (d.alive[p]) return p;
  }
  return from;
}

template <int NPlayers>
void advance_turn(CoupData<NPlayers>& d) {
  if (check_game_end(d)) return;
  d.active_player = next_alive_player(d, d.active_player);
  d.current_player = d.active_player;
  d.stage = CoupStage::kDeclareAction;
  d.declared_action = -1;
  d.action_target = -1;
  d.claimed_character = -1;
  d.challenger = -1;
  d.challenge_loser = -1;
  d.action_challenged = false;
  d.action_challenge_succeeded = false;
  d.blocker = -1;
  d.block_character = -1;
  d.counter_challenged = false;
  d.counter_challenge_succeeded = false;
  d.challenge_check_index = 0;
  d.exchange_drawn = {};
  d.exchange_held_count = 0;
}

bool is_challengeable_action(ActionId action) {
  return action == kTaxAction ||
         (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) ||
         (action >= kStealOffset && action < kStealOffset + kStealCount) ||
         action == kExchangeAction;
}

bool is_blockable_after_challenge(ActionId action) {
  return (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) ||
         (action >= kStealOffset && action < kStealOffset + kStealCount);
}

CharId claimed_character_for_action(ActionId action) {
  if (action == kTaxAction) return kDuke;
  if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) return kAssassin;
  if (action >= kStealOffset && action < kStealOffset + kStealCount) return kCaptain;
  if (action == kExchangeAction) return kAmbassador;
  return -1;
}

CharId claimed_character_for_block(ActionId block_action) {
  switch (block_action) {
    case kBlockDukeAction: return kDuke;
    case kBlockContessaAction: return kContessa;
    case kBlockAmbassadorAction: return kAmbassador;
    case kBlockCaptainAction: return kCaptain;
    default: return -1;
  }
}

template <int NPlayers>
int nth_alive_excluding(const CoupData<NPlayers>& d, int exclude, int n) {
  int count = 0;
  for (int i = 1; i <= NPlayers; ++i) {
    int p = (exclude + i) % NPlayers;
    if (p == exclude || !d.alive[p]) continue;
    if (count == n) return p;
    ++count;
  }
  return -1;
}

template <int NPlayers>
int alive_excluding_count(const CoupData<NPlayers>& d, int exclude) {
  int count = 0;
  for (int p = 0; p < NPlayers; ++p) {
    if (p != exclude && d.alive[p]) ++count;
  }
  return count;
}

template <int NPlayers>
void resolve_action_effects(CoupData<NPlayers>& d) {
  const ActionId action = d.declared_action;
  const int actor = d.active_player;
  const int target = d.action_target;

  if (action == kForeignAidAction) {
    d.coins[actor] += 2;
    advance_turn(d);
  } else if (action == kTaxAction) {
    d.coins[actor] += 3;
    advance_turn(d);
  } else if (action >= kStealOffset && action < kStealOffset + kStealCount) {
    if (target >= 0 && target < NPlayers && d.alive[target]) {
      int steal_amount = std::min(2, d.coins[target]);
      d.coins[target] -= steal_amount;
      d.coins[actor] += steal_amount;
    }
    advance_turn(d);
  } else if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) {
    if (target >= 0 && target < NPlayers && d.alive[target] && influence_count(d, target) > 0) {
      d.stage = CoupStage::kLoseInfluenceFromAction;
      d.current_player = target;
    } else {
      advance_turn(d);
    }
  } else if (action == kExchangeAction) {
    d.exchange_drawn[0] = draw_from_deck(d.court_deck, d.draw_nonce);
    d.exchange_drawn[1] = draw_from_deck(d.court_deck, d.draw_nonce);
    d.exchange_held_count = influence_count(d, actor) + 2;
    d.stage = CoupStage::kExchangeReturn1;
    d.current_player = actor;
  } else {
    advance_turn(d);
  }
}

template <int NPlayers>
void enter_counter_or_resolve(CoupData<NPlayers>& d) {
  const ActionId action = d.declared_action;

  if (action == kForeignAidAction) {
    d.stage = CoupStage::kCounterAction;
    d.challenge_check_index = 0;
    int first = nth_alive_excluding(d, d.active_player, 0);
    if (first >= 0) {
      d.current_player = first;
    } else {
      resolve_action_effects(d);
    }
  } else if (is_blockable_after_challenge(action)) {
    if (d.action_target >= 0 && d.action_target < NPlayers && d.alive[d.action_target] &&
        influence_count(d, d.action_target) > 0) {
      d.stage = CoupStage::kCounterAction;
      d.current_player = d.action_target;
    } else {
      resolve_action_effects(d);
    }
  } else {
    resolve_action_effects(d);
  }
}

template <int NPlayers>
std::vector<CharId> get_exchange_hand(const CoupData<NPlayers>& d) {
  std::vector<CharId> hand;
  int actor = d.active_player;
  for (int s = 0; s < 2; ++s) {
    if (!d.revealed[actor][s]) {
      hand.push_back(d.influence[actor][s]);
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (d.exchange_drawn[i] >= 0) {
      hand.push_back(d.exchange_drawn[i]);
    }
  }
  return hand;
}

}  // namespace

template <int NPlayers>
bool CoupRules<NPlayers>::validate_action(const IGameState& state, ActionId action) const {
  const auto& s = checked_cast<CoupState<NPlayers>>(state);
  if (s.data.terminal) return false;
  auto legal = legal_actions(state);
  return std::find(legal.begin(), legal.end(), action) != legal.end();
}

template <int NPlayers>
std::vector<ActionId> CoupRules<NPlayers>::legal_actions(const IGameState& state) const {
  const auto& s = checked_cast<CoupState<NPlayers>>(state);
  const auto& d = s.data;
  if (d.terminal) return {};

  std::vector<ActionId> actions;
  actions.reserve(16);

  switch (d.stage) {
    case CoupStage::kDeclareAction: {
      const int me = d.active_player;
      if (d.coins[me] >= kForceCoupThreshold) {
        for (int t = 0; t < NPlayers; ++t) {
          if (t != me && d.alive[t]) {
            actions.push_back(kCoupOffset + t);
          }
        }
        return actions;
      }
      actions.push_back(kIncomeAction);
      actions.push_back(kForeignAidAction);
      if (d.coins[me] >= kCoupCost) {
        for (int t = 0; t < NPlayers; ++t) {
          if (t != me && d.alive[t]) {
            actions.push_back(kCoupOffset + t);
          }
        }
      }
      actions.push_back(kTaxAction);
      if (d.coins[me] >= kAssassinateCost) {
        for (int t = 0; t < NPlayers; ++t) {
          if (t != me && d.alive[t]) {
            actions.push_back(kAssassinateOffset + t);
          }
        }
      }
      for (int t = 0; t < NPlayers; ++t) {
        if (t != me && d.alive[t]) {
          actions.push_back(kStealOffset + t);
        }
      }
      actions.push_back(kExchangeAction);
      break;
    }

    case CoupStage::kChallengeAction:
    case CoupStage::kChallengeCounter:
      actions.push_back(kChallengeAction);
      actions.push_back(kAllowAction);
      break;

    case CoupStage::kResolveChallengeAction:
    case CoupStage::kResolveChallengeCounter: {
      int p = d.current_player;
      for (int s = 0; s < 2; ++s) {
        if (!d.revealed[p][s]) {
          actions.push_back(s == 0 ? kRevealSlot0 : kRevealSlot1);
        }
      }
      break;
    }

    case CoupStage::kChooseLoseInfluence:
    case CoupStage::kChooseLoseInfluenceCounter:
    case CoupStage::kLoseInfluenceFromAction: {
      int p = d.current_player;
      for (int s = 0; s < 2; ++s) {
        if (!d.revealed[p][s]) {
          actions.push_back(s == 0 ? kLoseSlot0 : kLoseSlot1);
        }
      }
      break;
    }

    case CoupStage::kCounterAction: {
      const ActionId action = d.declared_action;
      if (action == kForeignAidAction) {
        actions.push_back(kBlockDukeAction);
        actions.push_back(kAllowNoBlockAction);
      } else if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) {
        actions.push_back(kBlockContessaAction);
        actions.push_back(kAllowNoBlockAction);
      } else if (action >= kStealOffset && action < kStealOffset + kStealCount) {
        actions.push_back(kBlockAmbassadorAction);
        actions.push_back(kBlockCaptainAction);
        actions.push_back(kAllowNoBlockAction);
      }
      break;
    }

    case CoupStage::kExchangeReturn1:
    case CoupStage::kExchangeReturn2: {
      auto hand = get_exchange_hand(d);
      std::array<bool, kCharacterCount> seen{};
      for (auto c : hand) {
        if (c >= 0 && c < kCharacterCount && !seen[static_cast<size_t>(c)]) {
          seen[static_cast<size_t>(c)] = true;
          actions.push_back(kReturnDuke + c);
        }
      }
      break;
    }
  }

  return actions;
}

template <int NPlayers>
UndoToken CoupRules<NPlayers>::do_action_fast(IGameState& state, ActionId action) const {
  auto& s = checked_cast<CoupState<NPlayers>>(state);
  auto& d = s.data;
  s.undo_stack.push_back(d);
  d.ply++;

  switch (d.stage) {
    case CoupStage::kDeclareAction: {
      d.declared_action = action;

      if (action == kIncomeAction) {
        d.coins[d.active_player] += 1;
        advance_turn(d);
      } else if (action == kForeignAidAction) {
        enter_counter_or_resolve(d);
      } else if (action >= kCoupOffset && action < kCoupOffset + kCoupCount) {
        int target = action - kCoupOffset;
        d.action_target = target;
        d.coins[d.active_player] -= kCoupCost;
        d.stage = CoupStage::kLoseInfluenceFromAction;
        d.current_player = target;
      } else if (action == kTaxAction ||
                 (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) ||
                 (action >= kStealOffset && action < kStealOffset + kStealCount) ||
                 action == kExchangeAction) {
        d.claimed_character = claimed_character_for_action(action);
        if (action >= kAssassinateOffset && action < kAssassinateOffset + kAssassinateCount) {
          d.action_target = action - kAssassinateOffset;
          d.coins[d.active_player] -= kAssassinateCost;
        } else if (action >= kStealOffset && action < kStealOffset + kStealCount) {
          d.action_target = action - kStealOffset;
        }
        d.stage = CoupStage::kChallengeAction;
        d.challenge_check_index = 0;
        int first = nth_alive_excluding(d, d.active_player, 0);
        if (first >= 0) {
          d.current_player = first;
        } else {
          enter_counter_or_resolve(d);
        }
      }
      break;
    }

    case CoupStage::kChallengeAction: {
      if (action == kChallengeAction) {
        d.challenger = d.current_player;
        d.action_challenged = true;
        d.stage = CoupStage::kResolveChallengeAction;
        d.current_player = d.active_player;
      } else {
        d.challenge_check_index++;
        int next = nth_alive_excluding(d, d.active_player, d.challenge_check_index);
        if (next >= 0) {
          d.current_player = next;
        } else {
          enter_counter_or_resolve(d);
        }
      }
      break;
    }

    case CoupStage::kResolveChallengeAction: {
      int slot = (action == kRevealSlot0) ? 0 : 1;
      CharId card = d.influence[d.active_player][slot];
      if (card == d.claimed_character) {
        d.court_deck.push_back(card);
        d.influence[d.active_player][slot] = draw_from_deck(d.court_deck, d.draw_nonce);
        d.action_challenge_succeeded = false;
        d.challenge_loser = d.challenger;
        if (influence_count(d, d.challenger) > 0) {
          d.stage = CoupStage::kChooseLoseInfluence;
          d.current_player = d.challenger;
        } else {
          check_game_end(d);
          if (!d.terminal) enter_counter_or_resolve(d);
        }
      } else {
        lose_influence_at_slot(d, d.active_player, slot);
        d.action_challenge_succeeded = true;
        if (check_game_end(d)) break;
        advance_turn(d);
      }
      break;
    }

    case CoupStage::kChooseLoseInfluence: {
      int slot = (action == kLoseSlot0) ? 0 : 1;
      lose_influence_at_slot(d, d.challenge_loser, slot);
      if (check_game_end(d)) break;
      enter_counter_or_resolve(d);
      break;
    }

    case CoupStage::kCounterAction: {
      if (action == kAllowNoBlockAction) {
        if (d.declared_action == kForeignAidAction) {
          d.challenge_check_index++;
          int next = nth_alive_excluding(d, d.active_player, d.challenge_check_index);
          if (next >= 0) {
            d.current_player = next;
          } else {
            resolve_action_effects(d);
          }
        } else {
          resolve_action_effects(d);
        }
      } else {
        d.blocker = d.current_player;
        d.block_character = claimed_character_for_block(action);
        d.stage = CoupStage::kChallengeCounter;
        d.challenge_check_index = 0;
        int first = nth_alive_excluding(d, d.blocker, 0);
        if (first >= 0) {
          d.current_player = first;
        } else {
          advance_turn(d);
        }
      }
      break;
    }

    case CoupStage::kChallengeCounter: {
      if (action == kChallengeAction) {
        d.challenger = d.current_player;
        d.counter_challenged = true;
        d.stage = CoupStage::kResolveChallengeCounter;
        d.current_player = d.blocker;
      } else {
        d.challenge_check_index++;
        int next = nth_alive_excluding(d, d.blocker, d.challenge_check_index);
        if (next >= 0) {
          d.current_player = next;
        } else {
          advance_turn(d);
        }
      }
      break;
    }

    case CoupStage::kResolveChallengeCounter: {
      int slot = (action == kRevealSlot0) ? 0 : 1;
      CharId card = d.influence[d.blocker][slot];
      if (card == d.block_character) {
        d.court_deck.push_back(card);
        d.influence[d.blocker][slot] = draw_from_deck(d.court_deck, d.draw_nonce);
        d.counter_challenge_succeeded = false;
        d.challenge_loser = d.challenger;
        if (influence_count(d, d.challenger) > 0) {
          d.stage = CoupStage::kChooseLoseInfluenceCounter;
          d.current_player = d.challenger;
        } else {
          if (!check_game_end(d)) advance_turn(d);
        }
      } else {
        lose_influence_at_slot(d, d.blocker, slot);
        d.counter_challenge_succeeded = true;
        if (check_game_end(d)) break;
        resolve_action_effects(d);
      }
      break;
    }

    case CoupStage::kChooseLoseInfluenceCounter: {
      int slot = (action == kLoseSlot0) ? 0 : 1;
      lose_influence_at_slot(d, d.challenge_loser, slot);
      if (check_game_end(d)) break;
      advance_turn(d);
      break;
    }

    case CoupStage::kExchangeReturn1: {
      CharId return_card = static_cast<CharId>(action - kReturnDuke);
      bool removed = false;
      for (int i = 0; i < 2; ++i) {
        if (d.exchange_drawn[i] == return_card && !removed) {
          d.exchange_drawn[i] = -1;
          removed = true;
        }
      }
      if (!removed) {
        int actor = d.active_player;
        for (int s = 0; s < 2; ++s) {
          if (!d.revealed[actor][s] && d.influence[actor][s] == return_card && !removed) {
            d.influence[actor][s] = -1;
            removed = true;
          }
        }
      }
      if (removed) {
        d.court_deck.push_back(return_card);
      }
      d.exchange_held_count--;
      d.stage = CoupStage::kExchangeReturn2;
      break;
    }

    case CoupStage::kExchangeReturn2: {
      CharId return_card = static_cast<CharId>(action - kReturnDuke);
      bool removed = false;
      for (int i = 0; i < 2; ++i) {
        if (d.exchange_drawn[i] == return_card && !removed) {
          d.exchange_drawn[i] = -1;
          removed = true;
        }
      }
      if (!removed) {
        int actor = d.active_player;
        for (int s = 0; s < 2; ++s) {
          if (!d.revealed[actor][s] && d.influence[actor][s] == return_card && !removed) {
            d.influence[actor][s] = -1;
            removed = true;
          }
        }
      }
      if (removed) {
        d.court_deck.push_back(return_card);
      }

      int actor = d.active_player;
      std::vector<CharId> remaining;
      for (int s = 0; s < 2; ++s) {
        if (!d.revealed[actor][s] && d.influence[actor][s] >= 0) {
          remaining.push_back(d.influence[actor][s]);
        }
      }
      for (int i = 0; i < 2; ++i) {
        if (d.exchange_drawn[i] >= 0) {
          remaining.push_back(d.exchange_drawn[i]);
        }
      }

      int ri = 0;
      for (int s = 0; s < 2; ++s) {
        if (!d.revealed[actor][s]) {
          d.influence[actor][s] = (ri < static_cast<int>(remaining.size()))
              ? remaining[static_cast<size_t>(ri++)] : -1;
        }
      }
      d.exchange_drawn = {-1, -1};
      d.exchange_held_count = 0;
      advance_turn(d);
      break;
    }

    case CoupStage::kLoseInfluenceFromAction: {
      int slot = (action == kLoseSlot0) ? 0 : 1;
      int target = d.current_player;
      lose_influence_at_slot(d, target, slot);
      if (check_game_end(d)) break;
      advance_turn(d);
      break;
    }
  }

  return UndoToken{static_cast<std::uint32_t>(s.undo_stack.size())};
}

template <int NPlayers>
void CoupRules<NPlayers>::undo_action(IGameState& state, const UndoToken& /*token*/) const {
  auto& s = checked_cast<CoupState<NPlayers>>(state);
  if (s.undo_stack.empty()) return;
  s.data = std::move(s.undo_stack.back());
  s.undo_stack.pop_back();
}

template class CoupRules<2>;
template class CoupRules<3>;
template class CoupRules<4>;

}  // namespace board_ai::coup
