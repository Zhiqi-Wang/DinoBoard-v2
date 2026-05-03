#include "splendor_rules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace board_ai::splendor {

namespace {

constexpr std::array<std::array<int, 3>, 10> kTakeThreeCombos{{
    {{0, 1, 2}}, {{0, 1, 3}}, {{0, 1, 4}}, {{0, 2, 3}}, {{0, 2, 4}},
    {{0, 3, 4}}, {{1, 2, 3}}, {{1, 2, 4}}, {{1, 3, 4}}, {{2, 3, 4}},
}};

constexpr std::array<std::array<int, 2>, 10> kTakeTwoDifferentCombos{{
    {{0, 1}}, {{0, 2}}, {{0, 3}}, {{0, 4}}, {{1, 2}},
    {{1, 3}}, {{1, 4}}, {{2, 3}}, {{2, 4}}, {{3, 4}},
}};

constexpr std::array<int, 5> kTakeOneColors{{0, 1, 2, 3, 4}};

template <int NPlayers>
std::int16_t draw_random_from_deck(SplendorData<NPlayers>& d, int tier_index) {
  auto& deck = d.decks[static_cast<size_t>(tier_index)];
  if (deck.empty()) return -1;

  if (d.forced_draw_override == -2) {
    return -1;
  }

  if (d.forced_draw_override >= 0) {
    const std::int16_t target = d.forced_draw_override;
    d.forced_draw_override = -1;
    for (size_t i = 0; i < deck.size(); ++i) {
      if (deck[i] == target) {
        if (i + 1 < deck.size()) deck[i] = deck.back();
        deck.pop_back();
        return target;
      }
    }
  }

  const std::uint64_t r = splitmix64(d.draw_nonce);
  const size_t idx = static_cast<size_t>(r % static_cast<std::uint64_t>(deck.size()));
  const std::int16_t picked = deck[idx];
  if (idx + 1 < deck.size()) {
    deck[idx] = deck.back();
  }
  deck.pop_back();
  return picked;
}

template <int NPlayers>
SplendorTurnStage stage_of(const SplendorData<NPlayers>& d) {
  return static_cast<SplendorTurnStage>(d.stage);
}

template <int NPlayers>
void set_stage(SplendorData<NPlayers>& d, SplendorTurnStage stage) {
  d.stage = static_cast<std::int8_t>(stage);
}

template <int NPlayers>
void clear_pending_nobles(SplendorData<NPlayers>& d) {
  d.pending_noble_slots.fill(-1);
  d.pending_nobles_size = 0;
}

template <int NPlayers>
bool can_afford(const SplendorData<NPlayers>& d, int player, const SplendorCard& card) {
  int need_gold = 0;
  for (int c = 0; c < kColorCount; ++c) {
    const int remaining =
        std::max(0, static_cast<int>(card.cost[static_cast<size_t>(c)]) -
                        static_cast<int>(d.player_bonuses[player][c]) -
                        static_cast<int>(d.player_gems[player][c]));
    need_gold += remaining;
  }
  return need_gold <= static_cast<int>(d.player_gems[player][5]);
}

template <int NPlayers>
bool noble_requirements_met(const SplendorData<NPlayers>& d, int player, int noble_slot) {
  if (noble_slot < 0 || noble_slot >= d.nobles_size) return false;
  const auto& nobles = splendor_nobles();
  const int nid = d.nobles[static_cast<size_t>(noble_slot)];
  if (nid < 0 || nid >= static_cast<int>(nobles.size())) return false;
  for (int c = 0; c < kColorCount; ++c) {
    if (d.player_bonuses[player][c] < nobles[static_cast<size_t>(nid)][static_cast<size_t>(c)]) {
      return false;
    }
  }
  return true;
}

template <int NPlayers>
bool pending_noble_slot_enabled(const SplendorData<NPlayers>& d, int noble_slot) {
  for (int i = 0; i < d.pending_nobles_size; ++i) {
    if (d.pending_noble_slots[static_cast<size_t>(i)] == noble_slot) return true;
  }
  return false;
}

template <int NPlayers>
bool populate_pending_nobles(SplendorData<NPlayers>& d, int player) {
  using Cfg = SplendorConfig<NPlayers>;
  clear_pending_nobles(d);
  for (int slot = 0; slot < d.nobles_size && slot < Cfg::kNobleCount; ++slot) {
    if (!noble_requirements_met(d, player, slot)) continue;
    d.pending_noble_slots[static_cast<size_t>(d.pending_nobles_size)] = static_cast<std::int8_t>(slot);
    d.pending_nobles_size += 1;
  }
  if (d.pending_nobles_size > 0) {
    set_stage(d, SplendorTurnStage::kChooseNoble);
    return true;
  }
  return false;
}

template <int NPlayers>
void claim_noble_at_slot(SplendorData<NPlayers>& d, int player, int noble_slot) {
  if (noble_slot < 0 || noble_slot >= d.nobles_size) return;
  if (!noble_requirements_met(d, player, noble_slot)) return;
  for (int j = noble_slot + 1; j < d.nobles_size; ++j) {
    d.nobles[static_cast<size_t>(j - 1)] = d.nobles[static_cast<size_t>(j)];
  }
  d.nobles[static_cast<size_t>(d.nobles_size - 1)] = -1;
  d.nobles_size -= 1;
  d.player_points[player] += 3;
  d.player_nobles_count[player] += 1;
  clear_pending_nobles(d);
}

template <int NPlayers>
int total_tokens_of_player(const SplendorData<NPlayers>& d, int player) {
  int total = 0;
  for (int i = 0; i < kTokenTypes; ++i) total += d.player_gems[player][static_cast<size_t>(i)];
  return total;
}

template <int NPlayers>
void update_pending_returns_state(SplendorData<NPlayers>& d, int player) {
  d.pending_returns = std::max(0, total_tokens_of_player(d, player) - 10);
  if (d.pending_returns > 0) {
    clear_pending_nobles(d);
    set_stage(d, SplendorTurnStage::kReturnTokens);
  } else if (stage_of(d) == SplendorTurnStage::kReturnTokens) {
    set_stage(d, SplendorTurnStage::kNormal);
  }
}

template <int NPlayers>
void draw_tableau(SplendorData<NPlayers>& d, int tier_index) {
  while (d.tableau_size[static_cast<size_t>(tier_index)] < 4 &&
         !d.decks[static_cast<size_t>(tier_index)].empty()) {
    const auto id = draw_random_from_deck(d, tier_index);
    d.tableau[static_cast<size_t>(tier_index)][static_cast<size_t>(d.tableau_size[static_cast<size_t>(tier_index)])] = id;
    d.tableau_size[static_cast<size_t>(tier_index)] += 1;
  }
}

template <int NPlayers>
void erase_tableau_slot(SplendorData<NPlayers>& d, int tier_index, int slot) {
  auto& row = d.tableau[static_cast<size_t>(tier_index)];
  const int n = d.tableau_size[static_cast<size_t>(tier_index)];
  for (int i = slot + 1; i < n; ++i) {
    row[static_cast<size_t>(i - 1)] = row[static_cast<size_t>(i)];
  }
  row[static_cast<size_t>(n - 1)] = -1;
  d.tableau_size[static_cast<size_t>(tier_index)] -= 1;
}

template <int NPlayers>
void remove_reserved_at(SplendorData<NPlayers>& d, int player, int idx) {
  auto& r = d.reserved[static_cast<size_t>(player)];
  auto& v = d.reserved_visible[static_cast<size_t>(player)];
  const int n = d.reserved_size[static_cast<size_t>(player)];
  for (int i = idx + 1; i < n; ++i) {
    r[static_cast<size_t>(i - 1)] = r[static_cast<size_t>(i)];
    v[static_cast<size_t>(i - 1)] = v[static_cast<size_t>(i)];
  }
  r[static_cast<size_t>(n - 1)] = -1;
  v[static_cast<size_t>(n - 1)] = 0;
  d.reserved_size[static_cast<size_t>(player)] -= 1;
}

template <int NPlayers>
void pay_for_card(SplendorData<NPlayers>& d, int player, const SplendorCard& card) {
  for (int c = 0; c < kColorCount; ++c) {
    int need = std::max(0, static_cast<int>(card.cost[static_cast<size_t>(c)]) -
                               static_cast<int>(d.player_bonuses[player][c]));
    const int use_color = std::min(need, static_cast<int>(d.player_gems[player][c]));
    d.player_gems[player][c] -= static_cast<std::int8_t>(use_color);
    d.bank[static_cast<size_t>(c)] += static_cast<std::int8_t>(use_color);
    need -= use_color;
    if (need > 0) {
      d.player_gems[player][5] -= static_cast<std::int8_t>(need);
      d.bank[5] += static_cast<std::int8_t>(need);
    }
  }
}

template <int NPlayers>
void update_terminal(SplendorData<NPlayers>& d, int actor) {
  using Cfg = SplendorConfig<NPlayers>;
  if (d.final_round_remaining < 0 && d.player_points[actor] >= kTargetPoints) {
    const int fp = static_cast<int>(d.first_player);
    d.final_round_remaining = (fp + Cfg::kPlayers - 1 - actor) % Cfg::kPlayers;
  } else if (d.final_round_remaining >= 0) {
    d.final_round_remaining -= 1;
  }
  if (d.plies >= kMaxPlies || d.final_round_remaining == 0) {
    d.terminal = true;
    int best_pts = -1;
    int best_cards = kMaxPlies;
    int winner = -1;
    bool tied = false;
    for (int p = 0; p < Cfg::kPlayers; ++p) {
      const int pts = d.player_points[p];
      const int cards = d.player_cards_count[p];
      if (pts > best_pts || (pts == best_pts && cards < best_cards)) {
        best_pts = pts;
        best_cards = cards;
        winner = p;
        tied = false;
      } else if (pts == best_pts && cards == best_cards) {
        tied = true;
      }
    }
    d.winner = tied ? -1 : winner;
    d.shared_victory = tied;
    d.scores.fill(0);
    if (!tied && winner >= 0) {
      for (int p = 0; p < Cfg::kPlayers; ++p) {
        d.scores[static_cast<size_t>(p)] = (p == winner) ? 1 : -1;
      }
    }
  } else {
    d.terminal = false;
    d.winner = -1;
    d.shared_victory = false;
    d.scores.fill(0);
  }
}

template <int NPlayers>
void finalize_turn(SplendorData<NPlayers>& d, int actor) {
  using Cfg = SplendorConfig<NPlayers>;
  set_stage(d, SplendorTurnStage::kNormal);
  d.pending_returns = 0;
  clear_pending_nobles(d);
  update_terminal(d, actor);
  d.current_player = (actor + 1) % Cfg::kPlayers;
}

}  // namespace


template <int NPlayers>
bool SplendorRules<NPlayers>::is_terminal_data(const SplendorData<NPlayers>& data) { return data.terminal; }

template <int NPlayers>
std::vector<ActionId> SplendorRules<NPlayers>::legal_actions_data(const SplendorData<NPlayers>& d) {
  std::vector<ActionId> out;
  if (d.terminal) return out;
  const int player = d.current_player;
  if (stage_of(d) == SplendorTurnStage::kReturnTokens || d.pending_returns > 0) {
    for (int c = 0; c < Cfg::kReturnTokenCount; ++c) {
      if (d.player_gems[player][static_cast<size_t>(c)] > 0) {
        out.push_back(Cfg::kReturnTokenOffset + c);
      }
    }
    if (out.empty()) out.push_back(Cfg::kPassAction);
    return out;
  }
  if (stage_of(d) == SplendorTurnStage::kChooseNoble) {
    for (int slot = 0; slot < Cfg::kChooseNobleCount; ++slot) {
      if (slot < d.nobles_size && pending_noble_slot_enabled(d, slot)) {
        out.push_back(Cfg::kChooseNobleOffset + slot);
      }
    }
    if (out.empty()) out.push_back(Cfg::kPassAction);
    return out;
  }
  const auto& cards = splendor_card_pool();

  for (int tier = 0; tier < 3; ++tier) {
    for (int slot = 0; slot < d.tableau_size[static_cast<size_t>(tier)]; ++slot) {
      const int id = d.tableau[static_cast<size_t>(tier)][static_cast<size_t>(slot)];
      if (id < 0 || id >= static_cast<int>(cards.size())) continue;
      if (can_afford(d, player, cards[static_cast<size_t>(id)])) {
        out.push_back(Cfg::kBuyFaceupOffset + tier * 4 + slot);
      }
    }
  }
  for (int i = 0; i < d.reserved_size[static_cast<size_t>(player)] && i < Cfg::kBuyReservedCount; ++i) {
    const int id = d.reserved[static_cast<size_t>(player)][static_cast<size_t>(i)];
    if (id < 0 || id >= static_cast<int>(cards.size())) continue;
    if (can_afford(d, player, cards[static_cast<size_t>(id)])) {
      out.push_back(Cfg::kBuyReservedOffset + i);
    }
  }
  if (d.reserved_size[static_cast<size_t>(player)] < 3) {
    for (int tier = 0; tier < 3; ++tier) {
      for (int slot = 0; slot < d.tableau_size[static_cast<size_t>(tier)] && slot < 4; ++slot) {
        out.push_back(Cfg::kReserveFaceupOffset + tier * 4 + slot);
      }
    }
    for (int tier = 0; tier < 3; ++tier) {
      if (!d.decks[static_cast<size_t>(tier)].empty()) {
        out.push_back(Cfg::kReserveDeckOffset + tier);
      }
    }
  }
  int available_token_colors = 0;
  for (int c = 0; c < kColorCount; ++c) {
    if (d.bank[static_cast<size_t>(c)] > 0) available_token_colors += 1;
  }
  for (int i = 0; i < Cfg::kTakeThreeCount; ++i) {
    const auto& comb = kTakeThreeCombos[static_cast<size_t>(i)];
    if (d.bank[static_cast<size_t>(comb[0])] > 0 && d.bank[static_cast<size_t>(comb[1])] > 0 &&
        d.bank[static_cast<size_t>(comb[2])] > 0) {
      out.push_back(Cfg::kTakeThreeOffset + i);
    }
  }
  if (available_token_colors < 3) {
    for (int i = 0; i < Cfg::kTakeTwoDifferentCount; ++i) {
      const auto& comb = kTakeTwoDifferentCombos[static_cast<size_t>(i)];
      if (d.bank[static_cast<size_t>(comb[0])] > 0 && d.bank[static_cast<size_t>(comb[1])] > 0) {
        out.push_back(Cfg::kTakeTwoDifferentOffset + i);
      }
    }
    for (int i = 0; i < Cfg::kTakeOneCount; ++i) {
      const int c = kTakeOneColors[static_cast<size_t>(i)];
      if (d.bank[static_cast<size_t>(c)] > 0) out.push_back(Cfg::kTakeOneOffset + i);
    }
  }
  for (int c = 0; c < Cfg::kTakeTwoSameCount; ++c) {
    if (d.bank[static_cast<size_t>(c)] >= 4) out.push_back(Cfg::kTakeTwoSameOffset + c);
  }
  if (out.empty()) out.push_back(Cfg::kPassAction);
  return out;
}

template <int NPlayers>
SplendorData<NPlayers> SplendorRules<NPlayers>::apply_action_copy(const SplendorData<NPlayers>& src, ActionId action) {
  SplendorData<NPlayers> d = src;
  if (d.terminal) return d;
  const auto legal = legal_actions_data(d);
  if (std::find(legal.begin(), legal.end(), action) == legal.end()) {
    action = Cfg::kPassAction;
  }

  const auto& cards = splendor_card_pool();
  const int player = d.current_player;
  bool bought_card = false;

  if (stage_of(d) == SplendorTurnStage::kReturnTokens || d.pending_returns > 0) {
    if (action >= Cfg::kReturnTokenOffset && action < Cfg::kReturnTokenOffset + Cfg::kReturnTokenCount) {
      const int token = action - Cfg::kReturnTokenOffset;
      if (d.player_gems[player][static_cast<size_t>(token)] > 0) {
        d.player_gems[player][static_cast<size_t>(token)] -= 1;
        d.bank[static_cast<size_t>(token)] += 1;
        d.pending_returns -= 1;
      }
    } else {
      int pick = 0;
      int best = -1;
      for (int c = 0; c < kTokenTypes; ++c) {
        if (d.player_gems[player][static_cast<size_t>(c)] > best) {
          best = d.player_gems[player][static_cast<size_t>(c)];
          pick = c;
        }
      }
      if (best > 0) {
        d.player_gems[player][static_cast<size_t>(pick)] -= 1;
        d.bank[static_cast<size_t>(pick)] += 1;
        d.pending_returns -= 1;
      } else {
        d.pending_returns = 0;
      }
    }
    d.plies += 1;
    if (d.pending_returns <= 0) {
      d.pending_returns = 0;
      set_stage(d, SplendorTurnStage::kNormal);
      finalize_turn(d, player);
    }
    return d;
  }
  if (stage_of(d) == SplendorTurnStage::kChooseNoble) {
    int chosen_slot = -1;
    if (action >= Cfg::kChooseNobleOffset && action < Cfg::kChooseNobleOffset + Cfg::kChooseNobleCount) {
      const int slot = action - Cfg::kChooseNobleOffset;
      if (pending_noble_slot_enabled(d, slot)) chosen_slot = slot;
    }
    if (chosen_slot < 0 && d.pending_nobles_size > 0) {
      chosen_slot = d.pending_noble_slots[0];
    }
    if (chosen_slot >= 0) {
      claim_noble_at_slot(d, player, chosen_slot);
    } else {
      clear_pending_nobles(d);
    }
    d.plies += 1;
    finalize_turn(d, player);
    return d;
  }

  if (action >= Cfg::kBuyFaceupOffset && action < Cfg::kBuyFaceupOffset + Cfg::kBuyFaceupCount) {
    const int local = action - Cfg::kBuyFaceupOffset;
    const int tier = local / 4;
    const int slot = local % 4;
    if (slot < d.tableau_size[static_cast<size_t>(tier)]) {
      const int cid = d.tableau[static_cast<size_t>(tier)][static_cast<size_t>(slot)];
      erase_tableau_slot(d, tier, slot);
      const auto& card = cards[static_cast<size_t>(cid)];
      pay_for_card(d, player, card);
      d.player_bonuses[player][static_cast<size_t>(card.bonus)] += 1;
      d.player_points[player] += card.points;
      d.player_cards_count[player] += 1;
      bought_card = true;
      draw_tableau(d, tier);
    }
  } else if (action >= Cfg::kBuyReservedOffset && action < Cfg::kBuyReservedOffset + Cfg::kBuyReservedCount) {
    const int idx = action - Cfg::kBuyReservedOffset;
    if (idx < d.reserved_size[static_cast<size_t>(player)]) {
      const int cid = d.reserved[static_cast<size_t>(player)][static_cast<size_t>(idx)];
      remove_reserved_at(d, player, idx);
      const auto& card = cards[static_cast<size_t>(cid)];
      pay_for_card(d, player, card);
      d.player_bonuses[player][static_cast<size_t>(card.bonus)] += 1;
      d.player_points[player] += card.points;
      d.player_cards_count[player] += 1;
      bought_card = true;
    }
  } else if (action >= Cfg::kReserveFaceupOffset && action < Cfg::kReserveFaceupOffset + Cfg::kReserveFaceupCount) {
    const int local = action - Cfg::kReserveFaceupOffset;
    const int tier = local / 4;
    const int slot = local % 4;
    if (d.reserved_size[static_cast<size_t>(player)] < 3 && slot < d.tableau_size[static_cast<size_t>(tier)]) {
      const int cid = d.tableau[static_cast<size_t>(tier)][static_cast<size_t>(slot)];
      erase_tableau_slot(d, tier, slot);
      const int idx = d.reserved_size[static_cast<size_t>(player)];
      d.reserved[static_cast<size_t>(player)][static_cast<size_t>(idx)] = static_cast<std::int16_t>(cid);
      d.reserved_visible[static_cast<size_t>(player)][static_cast<size_t>(idx)] = 1;
      d.reserved_size[static_cast<size_t>(player)] += 1;
      draw_tableau(d, tier);
      if (d.bank[5] > 0) {
        d.bank[5] -= 1;
        d.player_gems[player][5] += 1;
      }
    }
  } else if (action >= Cfg::kReserveDeckOffset && action < Cfg::kReserveDeckOffset + Cfg::kReserveDeckCount) {
    const int tier = action - Cfg::kReserveDeckOffset;
    if (d.reserved_size[static_cast<size_t>(player)] < 3 && !d.decks[static_cast<size_t>(tier)].empty()) {
      const int cid = draw_random_from_deck(d, tier);
      const int idx = d.reserved_size[static_cast<size_t>(player)];
      d.reserved[static_cast<size_t>(player)][static_cast<size_t>(idx)] = static_cast<std::int16_t>(cid);
      d.reserved_visible[static_cast<size_t>(player)][static_cast<size_t>(idx)] = 0;
      d.reserved_size[static_cast<size_t>(player)] += 1;
      if (d.bank[5] > 0) {
        d.bank[5] -= 1;
        d.player_gems[player][5] += 1;
      }
    }
  } else if (action >= Cfg::kTakeThreeOffset && action < Cfg::kTakeThreeOffset + Cfg::kTakeThreeCount) {
    const int idx = action - Cfg::kTakeThreeOffset;
    const auto& comb = kTakeThreeCombos[static_cast<size_t>(idx)];
    if (d.bank[static_cast<size_t>(comb[0])] > 0 && d.bank[static_cast<size_t>(comb[1])] > 0 &&
        d.bank[static_cast<size_t>(comb[2])] > 0) {
      for (int j = 0; j < 3; ++j) {
        const int c = comb[static_cast<size_t>(j)];
        d.bank[static_cast<size_t>(c)] -= 1;
        d.player_gems[player][static_cast<size_t>(c)] += 1;
      }
    }
  } else if (action >= Cfg::kTakeTwoDifferentOffset && action < Cfg::kTakeTwoDifferentOffset + Cfg::kTakeTwoDifferentCount) {
    const int idx = action - Cfg::kTakeTwoDifferentOffset;
    const auto& comb = kTakeTwoDifferentCombos[static_cast<size_t>(idx)];
    if (d.bank[static_cast<size_t>(comb[0])] > 0 && d.bank[static_cast<size_t>(comb[1])] > 0) {
      for (int j = 0; j < 2; ++j) {
        const int c = comb[static_cast<size_t>(j)];
        d.bank[static_cast<size_t>(c)] -= 1;
        d.player_gems[player][static_cast<size_t>(c)] += 1;
      }
    }
  } else if (action >= Cfg::kTakeOneOffset && action < Cfg::kTakeOneOffset + Cfg::kTakeOneCount) {
    const int c = kTakeOneColors[static_cast<size_t>(action - Cfg::kTakeOneOffset)];
    if (d.bank[static_cast<size_t>(c)] > 0) {
      d.bank[static_cast<size_t>(c)] -= 1;
      d.player_gems[player][static_cast<size_t>(c)] += 1;
    }
  } else if (action >= Cfg::kTakeTwoSameOffset && action < Cfg::kTakeTwoSameOffset + Cfg::kTakeTwoSameCount) {
    const int c = action - Cfg::kTakeTwoSameOffset;
    if (d.bank[static_cast<size_t>(c)] >= 4) {
      d.bank[static_cast<size_t>(c)] -= 2;
      d.player_gems[player][static_cast<size_t>(c)] += 2;
    }
  }

  d.plies += 1;
  update_pending_returns_state(d, player);
  if (d.pending_returns > 0) return d;
  if (bought_card && populate_pending_nobles(d, player)) return d;
  finalize_turn(d, player);
  return d;
}

template <int NPlayers>
bool SplendorRules<NPlayers>::validate_action(const IGameState& state, ActionId action) const {
  const auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  const auto legal = legal_actions_data(s->persistent.data());
  return std::find(legal.begin(), legal.end(), action) != legal.end();
}

template <int NPlayers>
std::vector<ActionId> SplendorRules<NPlayers>::legal_actions(const IGameState& state) const {
  const auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  return legal_actions_data(s->persistent.data());
}

template <int NPlayers>
UndoToken SplendorRules<NPlayers>::do_action_fast(IGameState& state, ActionId action) const {
  auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  UndoToken t{};
  t.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());
  s->undo_stack.push_back(s->persistent);
  if (validate_action(*s, action)) {
    s->persistent = s->persistent.advance(action);
  }
  return t;
}

template <int NPlayers>
UndoToken SplendorRules<NPlayers>::do_action_deterministic(IGameState& state, ActionId action) const {
  auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  UndoToken t{};
  t.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());
  s->undo_stack.push_back(s->persistent);
  if (validate_action(*s, action)) {
    auto data_copy = s->persistent.data();
    data_copy.forced_draw_override = -2;
    auto applied = apply_action_copy(data_copy, action);
    applied.forced_draw_override = -1;
    auto node = std::make_shared<SplendorPersistentNode<NPlayers>>();
    node->action_from_parent = action;
    node->materialized = std::make_shared<const SplendorData<NPlayers>>(std::move(applied));
    s->persistent = SplendorPersistentState<NPlayers>(std::move(node));
  }
  return t;
}

template <int NPlayers>
void SplendorRules<NPlayers>::undo_action(IGameState& state, const UndoToken& token) const {
  auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  if (s->undo_stack.empty()) return;
  s->persistent = s->undo_stack.back();
  s->undo_stack.pop_back();
  (void)token;
}

template <int NPlayers>
std::vector<ChanceOutcome> SplendorRules<NPlayers>::chance_outcomes(
    const IGameState& state, ActionId action) const {
  const auto& d = checked_cast<SplendorState<NPlayers>>(state).persistent.data();
  if (d.terminal) return {};
  if (stage_of(d) != SplendorTurnStage::kNormal) return {};

  int draw_tier = -1;
  if (action >= Cfg::kBuyFaceupOffset && action < Cfg::kBuyFaceupOffset + Cfg::kBuyFaceupCount) {
    const int local = action - Cfg::kBuyFaceupOffset;
    draw_tier = local / 4;
    const int slot = local % 4;
    if (slot >= d.tableau_size[static_cast<size_t>(draw_tier)]) return {};
    if (d.decks[static_cast<size_t>(draw_tier)].empty()) return {};
  } else if (action >= Cfg::kReserveFaceupOffset && action < Cfg::kReserveFaceupOffset + Cfg::kReserveFaceupCount) {
    const int local = action - Cfg::kReserveFaceupOffset;
    draw_tier = local / 4;
    const int slot = local % 4;
    if (slot >= d.tableau_size[static_cast<size_t>(draw_tier)]) return {};
    if (d.decks[static_cast<size_t>(draw_tier)].empty()) return {};
  } else if (action >= Cfg::kReserveDeckOffset && action < Cfg::kReserveDeckOffset + Cfg::kReserveDeckCount) {
    draw_tier = action - Cfg::kReserveDeckOffset;
    if (d.decks[static_cast<size_t>(draw_tier)].empty()) return {};
  } else {
    return {};
  }

  if (draw_tier < 0 || draw_tier >= 3) return {};
  const auto& deck = d.decks[static_cast<size_t>(draw_tier)];
  if (deck.empty()) return {};

  std::unordered_map<int, int> counts;
  for (auto cid : deck) counts[cid] += 1;

  const float total = static_cast<float>(deck.size());
  std::vector<ChanceOutcome> outcomes;
  outcomes.reserve(counts.size());
  for (const auto& [cid, cnt] : counts) {
    outcomes.push_back({cid, static_cast<float>(cnt) / total});
  }
  return outcomes;
}

template <int NPlayers>
UndoToken SplendorRules<NPlayers>::do_action_with_outcome(
    IGameState& state, ActionId action, int outcome_id) const {
  auto* s = &checked_cast<SplendorState<NPlayers>>(state);
  UndoToken t{};
  t.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());
  s->undo_stack.push_back(s->persistent);
  if (validate_action(*s, action)) {
    auto data_copy = s->persistent.data();
    data_copy.forced_draw_override = static_cast<std::int16_t>(outcome_id);
    auto applied = apply_action_copy(data_copy, action);
    applied.forced_draw_override = -1;
    auto node = std::make_shared<SplendorPersistentNode<NPlayers>>();
    node->action_from_parent = action;
    node->materialized = std::make_shared<SplendorData<NPlayers>>(std::move(applied));
    s->persistent = SplendorPersistentState<NPlayers>(std::move(node));
  }
  return t;
}

template class SplendorRules<2>;
template class SplendorRules<3>;
template class SplendorRules<4>;

}  // namespace board_ai::splendor
