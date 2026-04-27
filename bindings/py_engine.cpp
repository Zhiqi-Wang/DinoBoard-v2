#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../engine/core/game_interfaces.h"
#include "../engine/core/feature_encoder.h"
#include "../engine/infer/onnx_policy_value_evaluator.h"
#include "../engine/runtime/selfplay_runner.h"
#include "../engine/runtime/arena_runner.h"
#include "../engine/search/net_mcts.h"
#include "../engine/search/root_noise.h"
#include "../engine/search/temperature_schedule.h"

#ifdef DINOBOARD_GAME_TICTACTOE
#include "../games/tictactoe/tictactoe_state.h"
#include "../games/tictactoe/tictactoe_rules.h"
#include "../games/tictactoe/tictactoe_net_adapter.h"
#endif

#ifdef DINOBOARD_GAME_SPLENDOR
#include "../games/splendor/splendor_state.h"
#include "../games/splendor/splendor_rules.h"
#include "../games/splendor/splendor_net_adapter.h"
#endif

namespace py = pybind11;

using namespace board_ai;

namespace {

struct GameBundle {
  std::unique_ptr<IGameState> state;
  std::unique_ptr<IGameRules> rules;
  std::unique_ptr<IStateValueModel> value_model;
  std::unique_ptr<IFeatureEncoder> encoder;
  std::string game_id;
};

GameBundle create_game(const std::string& game_id, std::uint64_t seed) {
  GameBundle b;
  b.game_id = game_id;

#ifdef DINOBOARD_GAME_TICTACTOE
  if (game_id == "tictactoe") {
    auto s = std::make_unique<tictactoe::TicTacToeState>();
    s->reset_with_seed(seed);
    b.state = std::move(s);
    b.rules = std::make_unique<tictactoe::TicTacToeRules>();
    b.value_model = std::make_unique<tictactoe::TicTacToeStateValueModel>();
    b.encoder = std::make_unique<tictactoe::TicTacToeFeatureEncoder>();
    return b;
  }
#endif

#ifdef DINOBOARD_GAME_SPLENDOR
  if (game_id == "splendor") {
    auto s = std::make_unique<splendor::SplendorState>();
    s->reset_with_seed(seed);
    b.state = std::move(s);
    b.rules = std::make_unique<splendor::SplendorRules>();
    b.value_model = std::make_unique<splendor::SplendorStateValueModel>();
    b.encoder = std::make_unique<splendor::SplendorFeatureEncoder>();
    return b;
  }
#endif

  throw std::runtime_error("Unknown game: " + game_id);
}

py::dict run_selfplay_episode_py(
    const std::string& game_id,
    std::uint64_t seed,
    const std::string& model_path,
    int simulations,
    float c_puct,
    double temperature,
    double dirichlet_alpha,
    double dirichlet_epsilon,
    int dirichlet_on_first_n_plies,
    int max_game_plies) {
  auto bundle = create_game(game_id, seed);

  std::unique_ptr<search::IPolicyValueEvaluator> evaluator;
  if (!model_path.empty()) {
    evaluator = std::make_unique<infer::OnnxPolicyValueEvaluator>(
        model_path, bundle.encoder.get());
  }

  runtime::SelfplayConfig cfg{};
  cfg.simulations = simulations;
  cfg.c_puct = c_puct;
  cfg.temperature = temperature;
  cfg.dirichlet_alpha = dirichlet_alpha;
  cfg.dirichlet_epsilon = dirichlet_epsilon;
  cfg.dirichlet_on_first_n_plies = dirichlet_on_first_n_plies;
  cfg.max_game_plies = max_game_plies;

  search::IPolicyValueEvaluator* eval_ptr = evaluator.get();

  struct UniformEvaluator final : search::IPolicyValueEvaluator {
    bool evaluate(
        const IGameState&, int,
        const std::vector<ActionId>& legal,
        std::vector<float>* priors, float* value) const override {
      priors->assign(legal.size(), 1.0f / static_cast<float>(legal.size()));
      *value = 0.0f;
      return true;
    }
  };
  UniformEvaluator uniform_eval;
  if (!eval_ptr) eval_ptr = &uniform_eval;

  auto result = runtime::run_selfplay_episode(
      *bundle.state, *bundle.rules, *bundle.value_model, *eval_ptr, cfg, seed);

  py::list samples;
  for (const auto& s : result.samples) {
    py::dict sample;
    sample["ply"] = s.ply;
    sample["player"] = s.player;
    sample["action_id"] = s.action_id;
    sample["z"] = s.z;
    sample["policy_action_ids"] = s.policy_action_ids;
    sample["policy_action_visits"] = s.policy_action_visits;
    samples.append(sample);
  }

  py::dict out;
  out["winner"] = result.winner;
  out["draw"] = result.draw;
  out["total_plies"] = result.total_plies;
  out["samples"] = samples;
  return out;
}

py::dict run_arena_match_py(
    const std::string& game_id,
    std::uint64_t seed,
    const std::string& model_path_0,
    const std::string& model_path_1,
    int simulations_0,
    int simulations_1,
    double temperature) {
  auto bundle = create_game(game_id, seed);

  std::unique_ptr<search::IPolicyValueEvaluator> eval0;
  std::unique_ptr<search::IPolicyValueEvaluator> eval1;
  if (!model_path_0.empty()) {
    eval0 = std::make_unique<infer::OnnxPolicyValueEvaluator>(
        model_path_0, bundle.encoder.get());
  }
  if (!model_path_1.empty()) {
    eval1 = std::make_unique<infer::OnnxPolicyValueEvaluator>(
        model_path_1, bundle.encoder.get());
  }

  struct UniformEvaluator final : search::IPolicyValueEvaluator {
    bool evaluate(
        const IGameState&, int,
        const std::vector<ActionId>& legal,
        std::vector<float>* priors, float* value) const override {
      priors->assign(legal.size(), 1.0f / static_cast<float>(legal.size()));
      *value = 0.0f;
      return true;
    }
  };
  UniformEvaluator uniform_eval;

  search::IPolicyValueEvaluator* ptr0 = eval0 ? eval0.get() : &uniform_eval;
  search::IPolicyValueEvaluator* ptr1 = eval1 ? eval1.get() : &uniform_eval;

  runtime::ArenaPlayerConfig cfg0{};
  cfg0.simulations = simulations_0;
  cfg0.temperature = temperature;
  runtime::ArenaPlayerConfig cfg1{};
  cfg1.simulations = simulations_1;
  cfg1.temperature = temperature;

  auto result = runtime::run_arena_match(
      *bundle.state, *bundle.rules, *bundle.value_model,
      [&](int player) -> const search::IPolicyValueEvaluator& {
        return player == 0 ? *ptr0 : *ptr1;
      },
      cfg0, cfg1, 500, seed);

  py::dict out;
  out["winner"] = result.winner;
  out["draw"] = result.draw;
  out["total_plies"] = result.total_plies;
  return out;
}

py::dict encode_state_py(
    const std::string& game_id,
    std::uint64_t seed) {
  auto bundle = create_game(game_id, seed);
  const int player = bundle.state->current_player();
  const auto legal = bundle.rules->legal_actions(*bundle.state);

  std::vector<float> features;
  std::vector<float> legal_mask;
  bundle.encoder->encode(*bundle.state, player, legal, &features, &legal_mask);

  py::dict out;
  out["features"] = features;
  out["legal_mask"] = legal_mask;
  out["legal_actions"] = legal;
  out["current_player"] = player;
  out["is_terminal"] = bundle.state->is_terminal();
  out["action_space"] = bundle.encoder->action_space();
  out["feature_dim"] = bundle.encoder->feature_dim();
  return out;
}

}  // namespace

PYBIND11_MODULE(dinoboard_engine, m) {
  m.doc() = "DinoBoard C++ engine bindings";

  m.def("run_selfplay_episode", &run_selfplay_episode_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("model_path") = "",
      py::arg("simulations") = 200,
      py::arg("c_puct") = 1.4f,
      py::arg("temperature") = 1.0,
      py::arg("dirichlet_alpha") = 0.3,
      py::arg("dirichlet_epsilon") = 0.25,
      py::arg("dirichlet_on_first_n_plies") = 30,
      py::arg("max_game_plies") = 500);

  m.def("run_arena_match", &run_arena_match_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("model_path_0") = "",
      py::arg("model_path_1") = "",
      py::arg("simulations_0") = 200,
      py::arg("simulations_1") = 200,
      py::arg("temperature") = 0.0);

  m.def("encode_state", &encode_state_py,
      py::arg("game_id"),
      py::arg("seed") = 0xC0FFEE);

  m.def("available_games", []() -> std::vector<std::string> {
    std::vector<std::string> games;
#ifdef DINOBOARD_GAME_TICTACTOE
    games.push_back("tictactoe");
#endif
#ifdef DINOBOARD_GAME_SPLENDOR
    games.push_back("splendor");
#endif
    return games;
  });
}
