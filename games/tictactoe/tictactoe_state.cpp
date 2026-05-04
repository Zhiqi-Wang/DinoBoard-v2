#include "tictactoe_state.h"

namespace board_ai::tictactoe {

TicTacToeState::TicTacToeState() {
  reset_with_seed(0xC0FFEEu);
}

void TicTacToeState::reset_with_seed(std::uint64_t seed) {
  step_count_ = 0;
  current_player_ = 0;
  winner_ = -1;
  terminal = false;
  move_count = 0;
  scores = {0, 0};
  board.fill(static_cast<std::int8_t>(kEmptyCell));
  undo_stack.clear();
  rng_salt = sanitize_seed(seed);
}

StateHash64 TicTacToeState::state_hash(bool include_hidden_rng) const {
  std::size_t h = 0;
  hash_combine(h, static_cast<std::size_t>(current_player_));
  hash_combine(h, static_cast<std::size_t>(winner_ + 1));
  hash_combine(h, static_cast<std::size_t>(terminal ? 1 : 0));
  hash_combine(h, static_cast<std::size_t>(move_count));
  hash_combine(h, static_cast<std::size_t>(scores[0] + 2));
  hash_combine(h, static_cast<std::size_t>(scores[1] + 2));
  for (std::int8_t c : board) {
    hash_combine(h, static_cast<std::size_t>(c + 2));
  }
  if (include_hidden_rng) {
    hash_combine(h, static_cast<std::size_t>(rng_salt));
  }
  return static_cast<StateHash64>(h);
}

void TicTacToeState::hash_public_fields(Hasher& h) const {
  // TicTacToe is fully public: all board cells, scores, outcome are visible.
  h.add(current_player_);
  h.add(winner_ + 1);
  h.add(terminal ? 1 : 0);
  h.add(move_count);
  h.add(scores[0] + 2);
  h.add(scores[1] + 2);
  for (std::int8_t c : board) h.add(c + 2);
}

void TicTacToeState::hash_private_fields(int /*player*/, Hasher& /*h*/) const {
  // TicTacToe has no private info; nothing to hash per-player.
}

}  // namespace board_ai::tictactoe
