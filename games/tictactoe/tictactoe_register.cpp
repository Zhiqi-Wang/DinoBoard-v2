#include "../../engine/core/game_registry.h"
#include "tictactoe_state.h"
#include "tictactoe_rules.h"
#include "tictactoe_net_adapter.h"

namespace {

using board_ai::AnyMap;
using board_ai::tictactoe::TicTacToeState;
using board_ai::tictactoe::kBoardSize;
using board_ai::tictactoe::kPlayers;

AnyMap serialize_ttt(const board_ai::IGameState& state) {
  const auto& s = board_ai::checked_cast<TicTacToeState>(state);
  std::vector<int> board(kBoardSize);
  for (int i = 0; i < kBoardSize; ++i) board[static_cast<size_t>(i)] = static_cast<int>(s.board[static_cast<size_t>(i)]);
  std::vector<int> scores(kPlayers);
  for (int i = 0; i < kPlayers; ++i) scores[static_cast<size_t>(i)] = s.scores[static_cast<size_t>(i)];
  return {
      {"board", std::any(board)},
      {"current_player", std::any(s.current_player())},
      {"is_terminal", std::any(s.is_terminal())},
      {"winner", std::any(s.winner())},
      {"scores", std::any(scores)},
      {"move_count", std::any(s.move_count)},
  };
}

AnyMap describe_ttt(board_ai::ActionId a) {
  return {
      {"type", std::any(std::string("place"))},
      {"row", std::any(static_cast<int>(a) / 3)},
      {"col", std::any(static_cast<int>(a) % 3)},
      {"cell_index", std::any(static_cast<int>(a))},
  };
}

board_ai::GameRegistrar reg("tictactoe", [](std::uint64_t seed) {
  board_ai::GameBundle b;
  b.game_id = "tictactoe";
  auto s = std::make_unique<TicTacToeState>();
  s->reset_with_seed(seed);
  b.state = std::move(s);
  b.rules = std::make_unique<board_ai::tictactoe::TicTacToeRules>();
  b.value_model = std::make_unique<board_ai::DefaultStateValueModel>();
  b.encoder = std::make_unique<board_ai::tictactoe::TicTacToeFeatureEncoder>();
  b.state_serializer = serialize_ttt;
  b.action_descriptor = describe_ttt;
  return b;
});
}  // namespace
