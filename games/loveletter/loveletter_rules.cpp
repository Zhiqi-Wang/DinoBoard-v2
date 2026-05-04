#include "loveletter_rules.h"

#include <algorithm>
#include <cstdint>

namespace board_ai::loveletter {

namespace {

struct DecodedAction {
  std::int8_t card = 0;
  int target = -1;
  std::int8_t guess = 0;
};

DecodedAction decode_action(ActionId action) {
  DecodedAction r;
  if (action >= kGuardOffset && action < kGuardOffset + kGuardCount) {
    r.card = kGuard;
    int idx = action - kGuardOffset;
    r.target = idx / 7;
    r.guess = static_cast<std::int8_t>(idx % 7 + 2);
  } else if (action >= kPriestOffset && action < kPriestOffset + kPriestCount) {
    r.card = kPriest;
    r.target = action - kPriestOffset;
  } else if (action >= kBaronOffset && action < kBaronOffset + kBaronCount) {
    r.card = kBaron;
    r.target = action - kBaronOffset;
  } else if (action == kHandmaidAction) {
    r.card = kHandmaid;
  } else if (action >= kPrinceOffset && action < kPrinceOffset + kPrinceCount) {
    r.card = kPrince;
    r.target = action - kPrinceOffset;
  } else if (action >= kKingOffset && action < kKingOffset + kKingCount) {
    r.card = kKing;
    r.target = action - kKingOffset;
  } else if (action == kCountessAction) {
    r.card = kCountess;
  } else if (action == kPrincessAction) {
    r.card = kPrincess;
  }
  return r;
}

template <int NPlayers>
bool has_valid_target(const LoveLetterData<NPlayers>& d, int me) {
  for (int p = 0; p < NPlayers; ++p) {
    if (p == me) continue;
    if (d.alive[p] && !d.protected_flags[p]) return true;
  }
  return false;
}

template <int NPlayers>
void add_guard_actions(const LoveLetterData<NPlayers>& d, int me,
                       std::vector<ActionId>& actions) {
  bool found = false;
  for (int t = 0; t < NPlayers; ++t) {
    if (t == me || !d.alive[t] || d.protected_flags[t]) continue;
    for (int g = 2; g <= 8; ++g) {
      actions.push_back(kGuardOffset + t * 7 + (g - 2));
    }
    found = true;
  }
  if (!found) {
    actions.push_back(kGuardOffset + me * 7);
  }
}

template <int NPlayers>
void add_targeted_actions(const LoveLetterData<NPlayers>& d, int me,
                          int offset, bool allow_self,
                          std::vector<ActionId>& actions) {
  bool found = false;
  for (int t = 0; t < NPlayers; ++t) {
    if (!allow_self && t == me) continue;
    if (!d.alive[t] || (t != me && d.protected_flags[t])) continue;
    actions.push_back(offset + t);
    found = true;
  }
  if (!found) {
    actions.push_back(offset + me);
  }
}

template <int NPlayers>
void add_actions_for_card(const LoveLetterData<NPlayers>& d, int me,
                          std::int8_t card, std::vector<ActionId>& actions) {
  switch (card) {
    case kGuard:
      add_guard_actions(d, me, actions);
      break;
    case kPriest:
      add_targeted_actions(d, me, kPriestOffset, false, actions);
      break;
    case kBaron:
      add_targeted_actions(d, me, kBaronOffset, false, actions);
      break;
    case kHandmaid:
      actions.push_back(kHandmaidAction);
      break;
    case kPrince:
      add_targeted_actions(d, me, kPrinceOffset, true, actions);
      break;
    case kKing:
      add_targeted_actions(d, me, kKingOffset, false, actions);
      break;
    case kCountess:
      actions.push_back(kCountessAction);
      break;
    case kPrincess:
      actions.push_back(kPrincessAction);
      break;
    default:
      break;
  }
}

template <int NPlayers>
void eliminate_player(LoveLetterData<NPlayers>& d, int player) {
  d.alive[player] = 0;
  d.protected_flags[player] = 0;
  d.hand_exposed[player] = 0;
  if (d.hand[player] != 0) {
    d.discard_piles[static_cast<size_t>(player)].push_back(d.hand[player]);
    d.hand[player] = 0;
  }
}

template <int NPlayers>
int count_alive(const LoveLetterData<NPlayers>& d) {
  int n = 0;
  for (int p = 0; p < NPlayers; ++p) {
    if (d.alive[p]) ++n;
  }
  return n;
}

template <int NPlayers>
int sole_survivor(const LoveLetterData<NPlayers>& d) {
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
void check_end_game(LoveLetterData<NPlayers>& d) {
  int surv = sole_survivor(d);
  if (surv >= 0) {
    d.terminal = true;
    d.winner = surv;
    return;
  }

  if (d.deck.empty()) {
    int best_card = -1;
    int best_player = -1;
    bool tie = false;
    for (int p = 0; p < NPlayers; ++p) {
      if (!d.alive[p]) continue;
      int card = d.hand[p];
      if (card > best_card) {
        best_card = card;
        best_player = p;
        tie = false;
      } else if (card == best_card) {
        int sum_p = 0;
        for (auto c : d.discard_piles[static_cast<size_t>(p)]) sum_p += c;
        int sum_best = 0;
        for (auto c : d.discard_piles[static_cast<size_t>(best_player)]) sum_best += c;
        if (sum_p > sum_best) {
          best_player = p;
          tie = false;
        } else if (sum_p == sum_best) {
          tie = true;
        }
      }
    }
    d.terminal = true;
    d.winner = tie ? -1 : best_player;
  }
}

std::int8_t draw_from_deck_local(std::vector<std::int8_t>& deck, std::uint64_t& nonce) {
  if (deck.empty()) return 0;
  const std::uint64_t r = splitmix64(nonce);
  const size_t idx = static_cast<size_t>(r % static_cast<std::uint64_t>(deck.size()));
  const std::int8_t card = deck[idx];
  if (idx + 1 < deck.size()) {
    deck[idx] = deck.back();
  }
  deck.pop_back();
  return card;
}

template <int NPlayers>
int next_alive_player(const LoveLetterData<NPlayers>& d, int from) {
  for (int i = 1; i <= NPlayers; ++i) {
    int p = (from + i) % NPlayers;
    if (d.alive[p]) return p;
  }
  return from;
}

template <int NPlayers>
void advance_turn(LoveLetterData<NPlayers>& d) {
  check_end_game(d);
  if (d.terminal) return;

  int next = next_alive_player(d, d.current_player);
  d.current_player = next;

  d.protected_flags[static_cast<size_t>(next)] = 0;

  if (d.deck.empty()) {
    check_end_game(d);
    return;
  }

  d.drawn_card = draw_from_deck_local(d.deck, d.draw_nonce);
}

}  // namespace

template <int NPlayers>
bool LoveLetterRules<NPlayers>::validate_action(const IGameState& state, ActionId action) const {
  const auto& s = checked_cast<LoveLetterState<NPlayers>>(state);
  if (s.data.terminal) return false;
  auto legal = legal_actions(state);
  return std::find(legal.begin(), legal.end(), action) != legal.end();
}

template <int NPlayers>
std::vector<ActionId> LoveLetterRules<NPlayers>::legal_actions(const IGameState& state) const {
  const auto& s = checked_cast<LoveLetterState<NPlayers>>(state);
  const auto& d = s.data;
  if (d.terminal) return {};

  const int me = d.current_player;
  const std::int8_t h = d.hand[me];
  const std::int8_t dc = d.drawn_card;

  bool must_countess = false;
  if ((h == kCountess && (dc == kKing || dc == kPrince)) ||
      (dc == kCountess && (h == kKing || h == kPrince))) {
    must_countess = true;
  }

  if (must_countess) {
    return {kCountessAction};
  }

  std::vector<ActionId> actions;
  actions.reserve(16);

  add_actions_for_card(d, me, h, actions);

  if (dc != h) {
    add_actions_for_card(d, me, dc, actions);
  }

  return actions;
}

template <int NPlayers>
UndoToken LoveLetterRules<NPlayers>::do_action_fast(IGameState& state, ActionId action) const {
  auto& s = checked_cast<LoveLetterState<NPlayers>>(state);
  auto& d = s.data;

  s.undo_stack.push_back(d);
  s.begin_step();
  d.ply++;

  const DecodedAction act = decode_action(action);
  const int me = d.current_player;

  std::int8_t played_card = act.card;
  bool played_from_hand = (d.hand[me] == played_card);
  if (played_from_hand) {
    d.hand[me] = d.drawn_card;
  }
  d.drawn_card = 0;
  d.discard_piles[static_cast<size_t>(me)].push_back(played_card);

  if (played_from_hand) {
    d.hand_exposed[me] = 0;
  }

  int target = act.target;
  bool target_is_self = (target == me);
  bool target_protected = (target >= 0 && target < NPlayers &&
                           (d.protected_flags[target] || !d.alive[target]));
  bool no_effect = target_is_self && played_card != kPrince && played_card != kHandmaid &&
                   played_card != kCountess && played_card != kPrincess;

  if (!no_effect && !target_protected) {
    switch (played_card) {
      case kGuard:
        if (target >= 0 && target < NPlayers && d.alive[target] &&
            d.hand[target] == act.guess) {
          eliminate_player(d, target);
        }
        break;

      case kPriest:
        if (target >= 0 && target < NPlayers && d.alive[target]) {
          d.hand_exposed[target] = 1;
        }
        break;

      case kBaron:
        if (target >= 0 && target < NPlayers && d.alive[target]) {
          int my_card = d.hand[me];
          int their_card = d.hand[target];
          if (my_card < their_card) {
            eliminate_player(d, me);
          } else if (their_card < my_card) {
            eliminate_player(d, target);
          } else {
            d.hand_exposed[me] = 1;
            d.hand_exposed[target] = 1;
          }
        }
        break;

      case kHandmaid:
        d.protected_flags[me] = 1;
        break;

      case kPrince:
        if (target >= 0 && target < NPlayers && d.alive[target]) {
          std::int8_t discarded = d.hand[target];
          d.discard_piles[static_cast<size_t>(target)].push_back(discarded);
          if (discarded == kPrincess) {
            d.hand[target] = 0;
            eliminate_player(d, target);
          } else {
            if (!d.deck.empty()) {
              d.hand[target] = draw_from_deck_local(d.deck, d.draw_nonce);
            } else {
              d.hand[target] = d.set_aside_card;
              d.set_aside_card = 0;
            }
            d.hand_exposed[target] = 0;
          }
        }
        break;

      case kKing:
        if (target >= 0 && target < NPlayers && d.alive[target]) {
          std::swap(d.hand[me], d.hand[target]);
          d.hand_exposed[me] = 1;
          d.hand_exposed[target] = 1;
        }
        break;

      case kCountess:
        break;

      case kPrincess:
        eliminate_player(d, me);
        break;

      default:
        break;
    }
  }

  advance_turn(d);

  return UndoToken{static_cast<std::uint32_t>(s.undo_stack.size())};
}

template <int NPlayers>
void LoveLetterRules<NPlayers>::undo_action(IGameState& state, const UndoToken& /*token*/) const {
  auto& s = checked_cast<LoveLetterState<NPlayers>>(state);
  if (s.undo_stack.empty()) return;
  s.data = std::move(s.undo_stack.back());
  s.undo_stack.pop_back();
  s.end_step();
}

template class LoveLetterRules<2>;
template class LoveLetterRules<3>;
template class LoveLetterRules<4>;

}  // namespace board_ai::loveletter
