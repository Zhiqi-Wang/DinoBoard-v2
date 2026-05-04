#include "azul_rules.h"

#include <algorithm>

namespace board_ai::azul {

namespace {
constexpr int kFloorTarget = 5;
constexpr int kFloorPenalties[7] = {-1, -1, -2, -2, -2, -3, -3};
constexpr int kFirstPlayerToken = -2;
}

template <int NPlayers>
int AzulRules<NPlayers>::decode_source(ActionId action) {
  return static_cast<int>(action / (kColors * kTargetsPerColor));
}

template <int NPlayers>
int AzulRules<NPlayers>::decode_color(ActionId action) {
  return static_cast<int>((action / kTargetsPerColor) % kColors);
}

template <int NPlayers>
int AzulRules<NPlayers>::decode_target_line(ActionId action) {
  return static_cast<int>(action % kTargetsPerColor);
}

template <int NPlayers>
int AzulRules<NPlayers>::wall_col_for_color(int row, int color_idx) {
  return (color_idx + row) % kColors;
}

template <int NPlayers>
int AzulRules<NPlayers>::score_wall_placement(const PlayerState& p, int row, int col) {
  auto filled = [&](int r, int c) -> bool {
    if (r < 0 || r >= kRows || c < 0 || c >= kColors) {
      return false;
    }
    return ((p.wall_mask[r] >> c) & 1U) != 0U;
  };

  int horizontal = 1;
  for (int c = col - 1; c >= 0 && filled(row, c); --c) {
    horizontal += 1;
  }
  for (int c = col + 1; c < kColors && filled(row, c); ++c) {
    horizontal += 1;
  }

  int vertical = 1;
  for (int r = row - 1; r >= 0 && filled(r, col); --r) {
    vertical += 1;
  }
  for (int r = row + 1; r < kRows && filled(r, col); ++r) {
    vertical += 1;
  }

  if (horizontal == 1 && vertical == 1) {
    return 1;
  }
  if (horizontal > 1 && vertical > 1) {
    return horizontal + vertical;
  }
  return std::max(horizontal, vertical);
}

template <int NPlayers>
int AzulRules<NPlayers>::apply_final_bonus(AzulState<NPlayers>& state, int pid) {
  auto& p = state.players[pid];
  int row_bonus = 0;
  for (int r = 0; r < kRows; ++r) {
    bool full = true;
    for (int c = 0; c < kColors; ++c) {
      if (((p.wall_mask[r] >> c) & 1U) == 0U) {
        full = false;
        break;
      }
    }
    if (full) {
      row_bonus += 2;
    }
  }

  int col_bonus = 0;
  for (int c = 0; c < kColors; ++c) {
    bool full = true;
    for (int r = 0; r < kRows; ++r) {
      if (((p.wall_mask[r] >> c) & 1U) == 0U) {
        full = false;
        break;
      }
    }
    if (full) {
      col_bonus += 7;
    }
  }

  int color_set_bonus = 0;
  for (int color = 0; color < kColors; ++color) {
    bool full = true;
    for (int row = 0; row < kRows; ++row) {
      const int col = wall_col_for_color(row, color);
      if (((p.wall_mask[row] >> col) & 1U) == 0U) {
        full = false;
        break;
      }
    }
    if (full) {
      color_set_bonus += 10;
    }
  }
  return row_bonus + col_bonus + color_set_bonus;
}

template <int NPlayers>
bool AzulRules<NPlayers>::will_round_end_after_action(const AzulState<NPlayers>& state, ActionId action) {
  const int source = decode_source(action);
  const int color = decode_color(action);
  if (source < 0 || source > Cfg::kCenterSource || color < 0 || color >= kColors) {
    return false;
  }
  AzulState<NPlayers> tmp = state;
  if (source < Cfg::kFactories) {
    const auto src = tmp.factories[source];
    for (int c = 0; c < kColors; ++c) {
      tmp.factories[source][c] = 0;
      if (c != color) {
        tmp.center[c] = static_cast<std::uint8_t>(tmp.center[c] + src[c]);
      }
    }
  } else {
    tmp.center[color] = 0;
  }
  return tmp.all_sources_empty();
}

template <int NPlayers>
void AzulRules<NPlayers>::apply_round_settlement(AzulState<NPlayers>& state) {
  bool any_full_row = false;
  for (int pid = 0; pid < Cfg::kPlayers; ++pid) {
    auto& p = state.players[pid];
    int round_gain = 0;
    for (int row = 0; row < kRows; ++row) {
      const int cap = row + 1;
      if (p.line_len[row] == cap && p.line_color[row] >= 0) {
        const int color = p.line_color[row];
        const int col = wall_col_for_color(row, color);
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << col);
        if ((p.wall_mask[row] & bit) == 0) {
          p.wall_mask[row] = static_cast<std::uint8_t>(p.wall_mask[row] | bit);
          round_gain += score_wall_placement(p, row, col);
        }
        for (int i = 0; i < cap - 1; ++i) {
          state.box_lid.push_back(static_cast<std::int8_t>(color));
        }
        p.line_len[row] = 0;
        p.line_color[row] = -1;
      }
    }

    int penalty = 0;
    for (int i = 0; i < p.floor_count && i < 7; ++i) {
      penalty += kFloorPenalties[i];
      if (p.floor[i] >= 0 && p.floor[i] < kColors) {
        state.box_lid.push_back(p.floor[i]);
      }
      p.floor[i] = -1;
    }
    p.floor_count = 0;

    p.score = std::max(0, p.score + round_gain + penalty);
    state.scores[pid] = p.score;

    for (int r = 0; r < kRows; ++r) {
      if (p.wall_mask[r] == 0b11111U) {
        any_full_row = true;
        break;
      }
    }
  }

  if (any_full_row) {
    for (int pid = 0; pid < Cfg::kPlayers; ++pid) {
      const int bonus = apply_final_bonus(state, pid);
      state.players[pid].score += bonus;
      state.scores[pid] = state.players[pid].score;
    }

    int best_score = -1;
    int best_rows = -1;
    int winner = -1;
    bool tied = false;
    for (int pid = 0; pid < Cfg::kPlayers; ++pid) {
      int rows = 0;
      for (int r = 0; r < kRows; ++r) {
        if (state.players[pid].wall_mask[r] == 0b11111U) {
          rows += 1;
        }
      }
      if (state.scores[pid] > best_score ||
          (state.scores[pid] == best_score && rows > best_rows)) {
        best_score = state.scores[pid];
        best_rows = rows;
        winner = pid;
        tied = false;
      } else if (state.scores[pid] == best_score && rows == best_rows) {
        tied = true;
      }
    }
    state.winner_ = tied ? -1 : winner;
    state.shared_victory = tied;
    state.terminal = true;
    return;
  }

  state.round_index += 1;
  state.first_player_token_in_center = true;
  state.current_player_ = state.first_player_next_round;
  state.winner_ = -1;
  state.shared_victory = false;
  state.refill_factories_from_rng();
}

template <int NPlayers>
void AzulRules<NPlayers>::apply_action_no_undo(AzulState<NPlayers>& state, ActionId action) {
  const int source = decode_source(action);
  const int color = decode_color(action);
  const int target_line = decode_target_line(action);
  auto& p = state.players[state.current_player_];

  int picked = 0;
  if (source < Cfg::kFactories) {
    picked = state.factories[source][color];
    const auto src = state.factories[source];
    for (int c = 0; c < kColors; ++c) {
      state.factories[source][c] = 0;
      if (c != color) {
        state.center[c] = static_cast<std::uint8_t>(state.center[c] + src[c]);
      }
    }
  } else {
    picked = state.center[color];
    state.center[color] = 0;
    if (state.first_player_token_in_center) {
      state.first_player_token_in_center = false;
      state.first_player_next_round = state.current_player_;
      if (p.floor_count < 7) {
        p.floor[p.floor_count] = static_cast<std::int8_t>(kFirstPlayerToken);
        p.floor_count += 1;
      }
    }
  }
  if (picked <= 0) {
    return;
  }

  int to_floor = 0;
  if (target_line == kFloorTarget) {
    to_floor = picked;
  } else {
    const int cap = target_line + 1;
    const int free_slots = cap - p.line_len[target_line];
    const int to_line = std::max(0, std::min(free_slots, picked));
    if (to_line > 0) {
      if (p.line_len[target_line] == 0) {
        p.line_color[target_line] = static_cast<std::int8_t>(color);
      }
      p.line_len[target_line] = static_cast<std::uint8_t>(p.line_len[target_line] + to_line);
    }
    to_floor = picked - to_line;
  }

  for (int i = 0; i < to_floor && p.floor_count < 7; ++i) {
    p.floor[p.floor_count] = static_cast<std::int8_t>(color);
    p.floor_count += 1;
  }

  const bool round_ended = state.all_sources_empty();
  if (round_ended) {
    apply_round_settlement(state);
  } else if (!state.terminal) {
    state.current_player_ = (state.current_player_ + 1) % Cfg::kPlayers;
  }
}

template <int NPlayers>
bool AzulRules<NPlayers>::validate_action(const IGameState& state, ActionId action) const {
  const auto* s = &checked_cast<AzulState<NPlayers>>(state);
  if (!s || s->terminal || action < 0 || action >= Cfg::kActionSpace) {
    return false;
  }
  const auto legal = legal_actions(*s);
  return std::find(legal.begin(), legal.end(), action) != legal.end();
}

template <int NPlayers>
std::vector<ActionId> AzulRules<NPlayers>::legal_actions(const IGameState& state) const {
  const auto* s = &checked_cast<AzulState<NPlayers>>(state);
  if (!s || s->terminal) {
    return {};
  }
  const auto& p = s->players[s->current_player_];
  std::vector<ActionId> out;
  out.reserve(64);

  auto emit_from_source = [&](int source, const std::array<std::uint8_t, kColors>& counts) {
    for (int color = 0; color < kColors; ++color) {
      if (counts[color] == 0) {
        continue;
      }
      for (int row = 0; row < kRows; ++row) {
        const int cap = row + 1;
        const int line_len = p.line_len[row];
        if (line_len >= cap) {
          continue;
        }
        if (line_len > 0 && p.line_color[row] != color) {
          continue;
        }
        const int target_col = wall_col_for_color(row, color);
        const bool occupied = ((p.wall_mask[row] >> target_col) & 1U) != 0U;
        if (occupied) {
          continue;
        }
        const ActionId aid =
            static_cast<ActionId>(((source * kColors) + color) * kTargetsPerColor + row);
        out.push_back(aid);
      }
      const ActionId floor_aid =
          static_cast<ActionId>(((source * kColors) + color) * kTargetsPerColor + kFloorTarget);
      out.push_back(floor_aid);
    }
  };

  for (int source = 0; source < Cfg::kFactories; ++source) {
    emit_from_source(source, s->factories[source]);
  }
  emit_from_source(Cfg::kCenterSource, s->center);
  return out;
}

template <int NPlayers>
UndoToken AzulRules<NPlayers>::do_action_fast(IGameState& state, ActionId action) const {
  auto* s = &checked_cast<AzulState<NPlayers>>(state);
  if (!validate_action(*s, action)) {
    return UndoToken{};
  }
  s->begin_step();  // framework step counter; paired with end_step in undo_action

  UndoRecord<NPlayers> rec;
  rec.prev_current_player = s->current_player_;
  rec.prev_first_player_next_round = s->first_player_next_round;
  rec.prev_winner = s->winner_;
  rec.prev_round_index = s->round_index;
  rec.prev_terminal = s->terminal;
  rec.prev_first_player_token_in_center = s->first_player_token_in_center;
  rec.prev_shared_victory = s->shared_victory;
  rec.prev_scores = s->scores;
  rec.prev_center = s->center;
  rec.prev_rng_salt = s->rng_salt;
  rec.prev_player = s->players[s->current_player_];
  const int source = decode_source(action);
  if (source >= 0 && source < Cfg::kFactories) {
    rec.has_factory_source = true;
    rec.source_factory_idx = source;
    rec.prev_factory_source = s->factories[source];
  }
  rec.use_full_snapshot = will_round_end_after_action(*s, action);
  if (rec.use_full_snapshot) {
    rec.full_before.current_player = s->current_player_;
    rec.full_before.first_player_next_round = s->first_player_next_round;
    rec.full_before.winner = s->winner_;
    rec.full_before.round_index = s->round_index;
    rec.full_before.terminal = s->terminal;
    rec.full_before.first_player_token_in_center = s->first_player_token_in_center;
    rec.full_before.shared_victory = s->shared_victory;
    rec.full_before.scores = s->scores;
    rec.full_before.factories = s->factories;
    rec.full_before.center = s->center;
    rec.full_before.bag.assign(s->bag.begin(), s->bag.end());
    rec.full_before.box_lid.assign(s->box_lid.begin(), s->box_lid.end());
    rec.full_before.players = s->players;
    rec.full_before.rng_salt = s->rng_salt;
  }
  s->undo_stack.push_back(rec);

  UndoToken token{};
  token.undo_depth = static_cast<std::uint32_t>(s->undo_stack.size());
  apply_action_no_undo(*s, action);
  return token;
}

template <int NPlayers>
UndoToken AzulRules<NPlayers>::do_action_deterministic(IGameState& state, ActionId action) const {
  auto* s = &checked_cast<AzulState<NPlayers>>(state);
  const int prev_round = s->round_index;
  UndoToken token = do_action_fast(state, action);
  if (!s->terminal && s->round_index != prev_round) {
    s->terminal = true;
    s->winner_ = -1;
  }
  return token;
}

template <int NPlayers>
void AzulRules<NPlayers>::undo_action(IGameState& state, const UndoToken& token) const {
  auto* s = &checked_cast<AzulState<NPlayers>>(state);
  if (s->undo_stack.empty()) return;
  UndoRecord<NPlayers> rec = s->undo_stack.back();
  s->undo_stack.pop_back();
  if (rec.use_full_snapshot) {
    s->current_player_ = rec.full_before.current_player;
    s->first_player_next_round = rec.full_before.first_player_next_round;
    s->winner_ = rec.full_before.winner;
    s->round_index = rec.full_before.round_index;
    s->terminal = rec.full_before.terminal;
    s->first_player_token_in_center = rec.full_before.first_player_token_in_center;
    s->shared_victory = rec.full_before.shared_victory;
    s->scores = rec.full_before.scores;
    s->factories = rec.full_before.factories;
    s->center = rec.full_before.center;
    s->bag.assign(rec.full_before.bag.begin(), rec.full_before.bag.end());
    s->box_lid.assign(rec.full_before.box_lid.begin(), rec.full_before.box_lid.end());
    s->players = rec.full_before.players;
    s->rng_salt = rec.full_before.rng_salt;
    s->end_step();
    return;
  }
  s->current_player_ = rec.prev_current_player;
  s->first_player_next_round = rec.prev_first_player_next_round;
  s->winner_ = rec.prev_winner;
  s->round_index = rec.prev_round_index;
  s->terminal = rec.prev_terminal;
  s->first_player_token_in_center = rec.prev_first_player_token_in_center;
  s->shared_victory = rec.prev_shared_victory;
  s->scores = rec.prev_scores;
  s->center = rec.prev_center;
  s->players[s->current_player_] = rec.prev_player;
  s->rng_salt = rec.prev_rng_salt;
  if (rec.has_factory_source && rec.source_factory_idx >= 0 && rec.source_factory_idx < Cfg::kFactories) {
    s->factories[rec.source_factory_idx] = rec.prev_factory_source;
  }
  s->end_step();
  (void)token;
}

template class AzulRules<2>;
template class AzulRules<3>;
template class AzulRules<4>;

}  // namespace board_ai::azul
