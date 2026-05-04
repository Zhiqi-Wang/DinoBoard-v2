#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../engine/core/game_interfaces.h"
#include "../engine/core/game_registry.h"
#include "../engine/core/feature_encoder.h"
#include "../engine/infer/onnx_policy_value_evaluator.h"
#include "../engine/runtime/selfplay_runner.h"
#include "../engine/runtime/arena_runner.h"
#include "../engine/runtime/heuristic_runner.h"
#include "../engine/search/net_mcts.h"
#include "../engine/search/root_noise.h"
#include "../engine/search/temperature_schedule.h"

namespace py = pybind11;

using namespace board_ai;

namespace {

// Tracker adapter helpers: convert the old (state_before, action, state_after)
// trio into the tracker's new event-only input. Games with a registered
// public_event_extractor produce the event stream; games without one pass
// empty vectors (tracker becomes effectively a no-op for those games, which
// matches their pre-migration behavior since they had no hidden info).
inline void tracker_init(IBeliefTracker& bt, const GameBundle& bundle,
                         const IGameState& state, int perspective) {
  AnyMap obs;
  if (bundle.initial_observation_extractor) {
    obs = bundle.initial_observation_extractor(state, perspective);
  }
  bt.init(perspective, obs);
}

inline void tracker_observe(IBeliefTracker& bt, const GameBundle& bundle,
                            const IGameState& before, ActionId action,
                            const IGameState& after, int perspective) {
  PublicEventTrace trace;
  if (bundle.public_event_extractor) {
    trace = bundle.public_event_extractor(before, action, after, perspective);
  }
  const int actor = before.current_player();
  bt.observe_public_event(actor, action, trace.pre_events, trace.post_events);
}

std::any py_to_any(const py::handle& obj);

AnyMap py_dict_to_any_map(const py::dict& d) {
  AnyMap m;
  for (const auto& [k, v] : d) {
    const std::string key = py::cast<std::string>(k);
    m[key] = py_to_any(v);
  }
  return m;
}

std::any py_to_any(const py::handle& obj) {
  if (py::isinstance<py::bool_>(obj)) return py::cast<bool>(obj);
  if (py::isinstance<py::int_>(obj))  return py::cast<int>(obj);
  if (py::isinstance<py::float_>(obj)) return py::cast<double>(obj);
  if (py::isinstance<py::str>(obj))   return py::cast<std::string>(obj);
  if (py::isinstance<py::dict>(obj)) {
    return py_dict_to_any_map(py::reinterpret_borrow<py::dict>(obj));
  }
  if (py::isinstance<py::list>(obj) || py::isinstance<py::tuple>(obj)) {
    // Inspect first element to decide vector<int> vs vector<any>.
    py::sequence seq = py::reinterpret_borrow<py::sequence>(obj);
    bool all_int = !seq.empty() ? py::isinstance<py::int_>(seq[0]) : false;
    if (all_int) {
      for (const auto& item : seq) {
        if (!py::isinstance<py::int_>(item) || py::isinstance<py::bool_>(item)) {
          all_int = false;
          break;
        }
      }
    }
    if (all_int) {
      std::vector<int> out;
      out.reserve(seq.size());
      for (const auto& item : seq) out.push_back(py::cast<int>(item));
      return out;
    }
    std::vector<std::any> out;
    out.reserve(seq.size());
    for (const auto& item : seq) out.push_back(py_to_any(item));
    return out;
  }
  throw std::runtime_error("py_to_any: unsupported Python type");
}

py::object any_to_py(const std::any& val) {
  if (val.type() == typeid(int))
    return py::cast(std::any_cast<int>(val));
  if (val.type() == typeid(float))
    return py::cast(std::any_cast<float>(val));
  if (val.type() == typeid(double))
    return py::cast(std::any_cast<double>(val));
  if (val.type() == typeid(bool))
    return py::cast(std::any_cast<bool>(val));
  if (val.type() == typeid(std::string))
    return py::cast(std::any_cast<std::string>(val));
  if (val.type() == typeid(std::vector<int>))
    return py::cast(std::any_cast<std::vector<int>>(val));
  if (val.type() == typeid(std::vector<std::any>)) {
    py::list lst;
    for (const auto& item : std::any_cast<std::vector<std::any>>(val))
      lst.append(any_to_py(item));
    return lst;
  }
  if (val.type() == typeid(std::vector<AnyMap>)) {
    py::list lst;
    for (const auto& item : std::any_cast<std::vector<AnyMap>>(val)) {
      py::dict d;
      for (const auto& [k, v] : item) d[py::cast(k)] = any_to_py(v);
      lst.append(d);
    }
    return lst;
  }
  if (val.type() == typeid(AnyMap)) {
    py::dict d;
    for (const auto& [k, v] : std::any_cast<AnyMap>(val))
      d[py::cast(k)] = any_to_py(v);
    return d;
  }
  throw std::runtime_error(
      std::string("any_to_py: unsupported type: ") + val.type().name());
}

py::dict sample_to_py(const runtime::SelfplaySample& s) {
  py::dict d;
  d["ply"] = s.ply;
  d["player"] = s.player;
  d["action_id"] = s.action_id;
  d["z"] = s.z;
  d["z_values"] = s.z_values;
  d["policy_action_ids"] = s.policy_action_ids;
  d["policy_action_visits"] = s.policy_action_visits;
  d["features"] = s.features;
  d["legal_mask"] = s.legal_mask;
  d["tail_solved"] = s.tail_solved;
  d["auxiliary_score"] = s.auxiliary_score;
  return d;
}

py::dict result_to_py(const runtime::SelfplayEpisodeResult& result) {
  py::list samples;
  for (const auto& s : result.samples)
    samples.append(sample_to_py(s));

  py::dict out;
  out["winner"] = result.winner;
  out["draw"] = result.draw;
  out["total_plies"] = result.total_plies;
  out["samples"] = samples;
  out["tail_solve_attempts"] = result.tail_solve_attempts;
  out["tail_solve_completed"] = result.tail_solve_completed;
  out["tail_solve_successes"] = result.tail_solve_successes;
  out["tail_solve_total_ms"] = result.tail_solve_total_ms;

  if (!result.custom_stats.empty()) {
    py::dict stats;
    for (const auto& [k, v] : result.custom_stats) stats[py::cast(k)] = v;
    out["custom_stats"] = stats;
  }

  if (result.trace_enabled) {
    out["trace_perspective"] = result.trace_perspective;
    // initial_observation: game-defined AnyMap
    py::dict init_obs;
    for (const auto& [k, v] : result.initial_observation) init_obs[py::cast(k)] = any_to_py(v);
    out["initial_observation"] = init_obs;
    // initial_belief_snapshot: tracker's serialize() output
    py::dict init_bs;
    for (const auto& [k, v] : result.initial_belief_snapshot) init_bs[py::cast(k)] = any_to_py(v);
    out["initial_belief_snapshot"] = init_bs;
    // observation_trace: list of per-ply records
    py::list trace_list;
    for (const auto& t : result.observation_trace) {
      py::dict entry;
      entry["ply"] = t.ply;
      entry["actor"] = t.actor;
      entry["action"] = static_cast<int>(t.action);
      py::list pre;
      for (const auto& [kind, payload] : t.pre_events) {
        py::dict e;
        e["kind"] = kind;
        py::dict p;
        for (const auto& [pk, pv] : payload) p[py::cast(pk)] = any_to_py(pv);
        e["payload"] = p;
        pre.append(e);
      }
      entry["pre_events"] = pre;
      py::list post;
      for (const auto& [kind, payload] : t.post_events) {
        py::dict e;
        e["kind"] = kind;
        py::dict p;
        for (const auto& [pk, pv] : payload) p[py::cast(pk)] = any_to_py(pv);
        e["payload"] = p;
        post.append(e);
      }
      entry["post_events"] = post;
      py::dict bs;
      for (const auto& [k, v] : t.belief_snapshot_after) bs[py::cast(k)] = any_to_py(v);
      entry["belief_snapshot_after"] = bs;
      trace_list.append(entry);
    }
    out["observation_trace"] = trace_list;
  }
  return out;
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
    int max_game_plies,
    bool tail_solve_enabled,
    int tail_solve_start_ply,
    int tail_solve_depth_limit,
    std::int64_t tail_solve_node_budget,
    float tail_solve_margin_weight,
    double temperature_initial,
    double temperature_final,
    int temperature_decay_plies,
    double heuristic_guidance_ratio,
    double heuristic_temperature,
    double training_filter_ratio,
    bool nopeek_enabled,
    int trace_perspective) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);
  // For tracing we need a SECOND bundle (and its belief_tracker) dedicated
  // to the traced perspective. The primary bundle's tracker is re-init'd
  // every ply by run_selfplay_episode for the current acting player's MCTS.
  std::unique_ptr<GameBundle> trace_bundle;
  IBeliefTracker* trace_bt = nullptr;
  runtime::PublicEventExtractor trace_extractor;
  runtime::InitialObservationExtractor trace_obs_extractor;
  if (trace_perspective >= 0) {
    trace_bundle = std::make_unique<GameBundle>(
        GameRegistry::instance().create_game(game_id, seed));
    trace_bt = trace_bundle->belief_tracker.get();
    trace_extractor = bundle.public_event_extractor;
    trace_obs_extractor = bundle.initial_observation_extractor;
    if (!trace_bt || !trace_extractor) {
      py::gil_scoped_acquire acquire;
      throw std::runtime_error(
          "run_selfplay_episode: trace_perspective >= 0 but game '" + game_id +
          "' did not register a belief_tracker + public_event_extractor");
    }
  }

  if (model_path.empty()) {
    throw std::invalid_argument("run_selfplay_episode: model_path must not be empty");
  }
  auto evaluator = std::make_unique<infer::OnnxPolicyValueEvaluator>(
      model_path, bundle.encoder.get());
  if (!evaluator->is_ready()) {
    throw std::runtime_error("run_selfplay_episode: failed to load model: " + evaluator->last_error());
  }
  const search::IPolicyValueEvaluator* eval_ptr = evaluator.get();

  runtime::SelfplayConfig cfg{};
  cfg.simulations = simulations;
  cfg.c_puct = c_puct;
  cfg.temperature = temperature;
  cfg.dirichlet_alpha = dirichlet_alpha;
  cfg.dirichlet_epsilon = dirichlet_epsilon;
  cfg.dirichlet_on_first_n_plies = dirichlet_on_first_n_plies;
  cfg.max_game_plies = max_game_plies;
  cfg.tail_solve_enabled = tail_solve_enabled;
  cfg.tail_solve_start_ply = tail_solve_start_ply;
  cfg.tail_solve_config.depth_limit = tail_solve_depth_limit;
  cfg.tail_solve_config.node_budget = tail_solve_node_budget;
  cfg.tail_solve_config.margin_weight = tail_solve_margin_weight;
  cfg.heuristic_guidance_ratio = heuristic_guidance_ratio;
  cfg.heuristic_temperature = heuristic_temperature;
  cfg.training_filter_ratio = training_filter_ratio;

  if (temperature_initial >= 0.0 || temperature_final >= 0.0) {
    cfg.temperature_schedule.enabled = true;
    if (temperature_initial >= 0.0) {
      cfg.temperature_schedule.has_initial = true;
      cfg.temperature_schedule.initial = temperature_initial;
    }
    if (temperature_final >= 0.0) {
      cfg.temperature_schedule.has_final = true;
      cfg.temperature_schedule.final_ = temperature_final;
    }
    cfg.temperature_schedule.decay_plies = temperature_decay_plies;
  }

  IBeliefTracker* bt = bundle.belief_tracker.get();
  // nopeek_enabled is the legacy selfplay peek-disable flag. In ISMCTS
  // the "peek" semantics moves to: when nopeek_enabled is FALSE, skip
  // root-sampling entirely (MCTS sees truth). Peek is useful as a training-
  // early-stage enhancement where value head learns from omniscient rollouts.
  if (!nopeek_enabled) {
    bt = nullptr;  // let MCTS see truth without root-sampling
  }

  auto result = runtime::run_selfplay_episode(
      *bundle.state, *bundle.rules, *bundle.value_model, *eval_ptr, cfg, seed,
      bt,
      bundle.encoder.get(),
      bundle.tail_solver.get(),
      bundle.adjudicator,
      bundle.auxiliary_scorer,
      bundle.heuristic_picker,
      bundle.training_action_filter,
      bundle.tail_solve_trigger,
      bundle.episode_stats_extractor,
      trace_perspective,
      trace_bt,
      trace_extractor,
      trace_obs_extractor);

  py::gil_scoped_acquire acquire;
  return result_to_py(result);
}

py::dict run_arena_match_py(
    const std::string& game_id,
    std::uint64_t seed,
    const std::vector<std::string>& model_paths,
    const std::vector<int>& simulations_list,
    double temperature,
    int max_game_plies,
    bool tail_solve) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);

  if (model_paths.empty()) {
    throw std::invalid_argument("run_arena_match: model_paths must not be empty");
  }
  for (size_t i = 0; i < model_paths.size(); ++i) {
    if (model_paths[i].empty()) {
      throw std::invalid_argument(
          "run_arena_match: model_paths[" + std::to_string(i) + "] must not be empty");
    }
  }

  std::vector<std::unique_ptr<infer::OnnxPolicyValueEvaluator>> evaluators;
  std::vector<const search::IPolicyValueEvaluator*> eval_ptrs;
  for (size_t i = 0; i < model_paths.size(); ++i) {
    auto ev = std::make_unique<infer::OnnxPolicyValueEvaluator>(
        model_paths[i], bundle.encoder.get());
    if (!ev->is_ready()) {
      throw std::runtime_error(
          "run_arena_match: failed to load model_" + std::to_string(i) + ": " + ev->last_error());
    }
    eval_ptrs.push_back(ev.get());
    evaluators.push_back(std::move(ev));
  }

  std::vector<runtime::ArenaPlayerConfig> player_configs;
  for (size_t i = 0; i < model_paths.size(); ++i) {
    runtime::ArenaPlayerConfig cfg{};
    cfg.simulations = (i < simulations_list.size())
        ? simulations_list[i] : 200;
    cfg.temperature = temperature;
    if (tail_solve && bundle.tail_solver) {
      cfg.tail_solve_enabled = true;
      cfg.tail_solve_config.depth_limit = 10;
      cfg.tail_solve_config.node_budget = 200000;
      cfg.tail_solver = bundle.tail_solver.get();
      cfg.tail_solve_trigger = bundle.tail_solve_trigger;
    }
    player_configs.push_back(cfg);
  }

  IBeliefTracker* arena_bt = bundle.belief_tracker.get();
  const size_t n_eval = eval_ptrs.size();
  auto result = runtime::run_arena_match(
      *bundle.state, *bundle.rules, *bundle.value_model,
      [&eval_ptrs, n_eval](int player) -> const search::IPolicyValueEvaluator& {
        return *eval_ptrs[static_cast<size_t>(player) % n_eval];
      },
      player_configs, max_game_plies, seed,
      arena_bt, bundle.adjudicator,
      bundle.public_event_extractor,
      bundle.initial_observation_extractor);

  py::gil_scoped_acquire acquire;
  py::dict out;
  out["winner"] = result.winner;
  out["draw"] = result.draw;
  out["total_plies"] = result.total_plies;
  py::list actions;
  for (auto a : result.action_history) actions.append(static_cast<int>(a));
  out["action_history"] = actions;
  py::list ply_stats;
  for (const auto& ps : result.ply_stats) {
    py::dict d;
    d["tail_solved"] = ps.tail_solved;
    d["tail_solve_value"] = ps.tail_solve_value;
    ply_stats.append(d);
  }
  out["ply_stats"] = ply_stats;
  return out;
}

py::dict run_constrained_eval_vs_heuristic_py(
    const std::string& game_id,
    std::uint64_t seed,
    const std::string& model_path,
    int simulations,
    int model_is_player,
    bool constrained,
    double heuristic_temperature) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);

  if (model_path.empty()) {
    throw std::invalid_argument("run_constrained_eval_vs_heuristic: model_path must not be empty");
  }
  auto model_eval = std::make_unique<infer::OnnxPolicyValueEvaluator>(
      model_path, bundle.encoder.get());
  if (!model_eval->is_ready()) {
    throw std::runtime_error("run_constrained_eval_vs_heuristic: failed to load model: " + model_eval->last_error());
  }
  const search::IPolicyValueEvaluator* eval_ptr = model_eval.get();

  std::unique_ptr<runtime::FilteredRulesWrapper> filtered_rules;
  if (constrained && bundle.training_action_filter) {
    filtered_rules = std::make_unique<runtime::FilteredRulesWrapper>(
        *bundle.rules, bundle.training_action_filter);
  }

  auto state = bundle.state->clone_state();
  int ply = 0;
  std::vector<ActionId> action_history;
  std::vector<runtime::ArenaPlyStats> ply_stats_vec;

  std::mt19937_64 rng(seed ^ 0xBEEF);
  IBeliefTracker* bt = bundle.belief_tracker.get();

  while (!state->is_terminal() && ply < 500) {
    const int cp = state->current_player();

    if (cp == model_is_player) {
      const IGameRules& rules_for_model =
          filtered_rules ? *filtered_rules : *bundle.rules;
      const auto legal = rules_for_model.legal_actions(*state);
      if (legal.empty()) break;

      if (bt) tracker_init(*bt, bundle, *state, cp);

      search::NetMctsConfig mcts_cfg{};
      mcts_cfg.simulations = simulations;
      mcts_cfg.c_puct = 1.4f;
      if (bt) {
        mcts_cfg.root_belief_tracker = bt;
      }

      if (bundle.tail_solver) {
        bool try_ts = false;
        if (bundle.tail_solve_trigger) {
          try_ts = bundle.tail_solve_trigger(*state, ply);
        } else if (ply >= 20) {
          try_ts = true;
        }
        if (try_ts) {
          mcts_cfg.tail_solve_enabled = true;
          mcts_cfg.tail_solve_config.depth_limit = 10;
          mcts_cfg.tail_solve_config.node_budget = 200000;
          mcts_cfg.tail_solver = bundle.tail_solver.get();
        }
      }

      search::NetMcts mcts(mcts_cfg);
      search::NetMctsStats stats{};
      const std::uint64_t mcts_seed = seed ^
          (static_cast<std::uint64_t>(ply) * kGoldenRatio64) ^ 0x243F6A8885A308D3ULL;
      mcts.search_root(*state, rules_for_model, *bundle.value_model,
                        *eval_ptr, &stats, mcts_seed);

      ActionId chosen = search::select_action_from_visits(
          stats.root_actions, stats.root_action_visits, 0.0,
          seed ^ static_cast<std::uint64_t>(ply), legal[0]);

      action_history.push_back(chosen);
      ply_stats_vec.push_back({stats.tail_solved, stats.tail_solve_value});
      std::unique_ptr<IGameState> state_before;
      if (bt) state_before = state->clone_state();
      bundle.rules->do_action_fast(*state, chosen);
      if (bt) tracker_observe(*bt, bundle, *state_before, chosen, *state, cp);
    } else {
      if (!bundle.heuristic_picker) break;
      auto hr = bundle.heuristic_picker(*state, *bundle.rules, rng());
      if (hr.actions.empty()) break;

      double u01 = static_cast<double>(rng() & 0xFFFFFFFF) / 4294967296.0;
      std::size_t idx = runtime::sample_heuristic_index(
          hr.scores, heuristic_temperature, u01);
      ActionId chosen = hr.actions[idx];
      action_history.push_back(chosen);
      ply_stats_vec.push_back({false, 0.0f});
      std::unique_ptr<IGameState> state_before;
      if (bt) state_before = state->clone_state();
      bundle.rules->do_action_fast(*state, chosen);
      if (bt) tracker_observe(*bt, bundle, *state_before, chosen, *state, cp);
    }
    ++ply;
  }

  int winner = -1;
  bool draw = true;
  if (state->is_terminal()) {
    winner = state->winner();
    draw = (winner < 0);
  } else if (bundle.adjudicator) {
    winner = bundle.adjudicator(*state);
    draw = (winner < 0);
  }

  py::gil_scoped_acquire acquire;
  py::dict out;
  out["winner"] = winner;
  out["draw"] = draw;
  out["total_plies"] = ply;
  py::list actions;
  for (auto a : action_history) actions.append(static_cast<int>(a));
  out["action_history"] = actions;
  py::list pstats;
  for (const auto& ps : ply_stats_vec) {
    py::dict d;
    d["tail_solved"] = ps.tail_solved;
    d["tail_solve_value"] = ps.tail_solve_value;
    pstats.append(d);
  }
  out["ply_stats"] = pstats;
  return out;
}

py::dict run_heuristic_episode_py(
    const std::string& game_id,
    std::uint64_t seed,
    double temperature,
    int max_game_plies) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);

  auto result = runtime::run_heuristic_episode(
      *bundle.state, *bundle.rules, *bundle.value_model,
      bundle.encoder.get(),
      bundle.heuristic_picker,
      temperature, max_game_plies, seed,
      bundle.auxiliary_scorer, bundle.adjudicator);

  py::gil_scoped_acquire acquire;
  return result_to_py(result);
}

py::dict encode_state_py(
    const std::string& game_id,
    std::uint64_t seed) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);
  const int player = bundle.state->current_player();
  const auto legal = bundle.rules->legal_actions(*bundle.state);

  std::vector<float> features;
  std::vector<float> legal_mask;
  bundle.encoder->encode(*bundle.state, player, legal, &features, &legal_mask);

  const bool is_terminal = bundle.state->is_terminal();
  const int action_space = bundle.encoder->action_space();
  const int feature_dim = bundle.encoder->feature_dim();

  // Also compute the public/private split so tests can verify the
  // structural invariant (changing an opp's private shouldn't affect
  // encode_private for perspective, etc.).
  std::vector<float> public_features, private_features;
  bundle.encoder->encode_public(*bundle.state, player, &public_features);
  bundle.encoder->encode_private(*bundle.state, player, &private_features);
  const int public_dim = bundle.encoder->public_feature_dim();
  const int private_dim = bundle.encoder->private_feature_dim();

  py::gil_scoped_acquire acquire;
  py::dict out;
  out["features"] = features;
  out["public_features"] = public_features;
  out["private_features"] = private_features;
  out["legal_mask"] = legal_mask;
  out["legal_actions"] = legal;
  out["current_player"] = player;
  out["is_terminal"] = is_terminal;
  out["action_space"] = action_space;
  out["feature_dim"] = feature_dim;
  out["public_feature_dim"] = public_dim;
  out["private_feature_dim"] = private_dim;
  return out;
}

py::dict tail_solve_py(
    const std::string& game_id,
    std::uint64_t seed,
    int perspective_player,
    int depth_limit,
    std::int64_t node_budget) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);
  if (!bundle.tail_solver) {
    py::gil_scoped_acquire acquire;
    throw std::runtime_error("tail_solve: game '" + game_id + "' has no tail_solver registered");
  }

  search::TailSolveConfig cfg{};
  cfg.depth_limit = depth_limit;
  cfg.node_budget = node_budget;
  if (bundle.auxiliary_scorer) {
    cfg.margin_weight = 0.01f;
    cfg.margin_scorer = bundle.auxiliary_scorer;
  }

  auto ts = bundle.tail_solver->solve(
      *bundle.state, *bundle.rules, *bundle.value_model,
      perspective_player, cfg);

  py::gil_scoped_acquire acquire;
  py::dict out;
  out["value"] = ts.value;
  out["best_action"] = ts.best_action;
  out["nodes_searched"] = ts.nodes_searched;
  out["elapsed_ms"] = ts.elapsed_ms;
  out["budget_exceeded"] = ts.budget_exceeded;
  return out;
}

class GameSessionWrapper {
 public:
  GameSessionWrapper(const std::string& game_id, std::uint64_t seed,
                     const std::string& model_path, bool use_filter)
      : game_id_(game_id), seed_(seed), model_path_(model_path) {
    py::gil_scoped_release release;
    bundle_ = std::make_unique<GameBundle>(
        GameRegistry::instance().create_game(game_id, seed));
    if (!model_path.empty()) {
      evaluator_ = std::make_unique<infer::OnnxPolicyValueEvaluator>(
          model_path, bundle_->encoder.get());
      if (!evaluator_->is_ready()) {
        throw std::runtime_error("GameSession: failed to load model: " + evaluator_->last_error());
      }
    }
    if (use_filter && bundle_->training_action_filter) {
      filtered_rules_ = std::make_unique<runtime::FilteredRulesWrapper>(
          *bundle_->rules, bundle_->training_action_filter);
    }
    bt_ = bundle_->belief_tracker.get();
    // Per-perspective AI views: MCTS searches on these, never on the truth
    // state (bundle_->state). Skipped when external_obs_mode_ is set
    // (AI API path) — in that mode bundle_->state IS the AI view.
    init_ai_views_();
  }

  void init_ai_views_() {
    if (external_obs_mode_) return;
    const int n = bundle_->state->num_players();
    ai_views_.resize(static_cast<size_t>(n));
    ai_trackers_.resize(static_cast<size_t>(n));
    ai_encoders_.resize(static_cast<size_t>(n));
    ai_evaluators_.resize(static_cast<size_t>(n));
    for (int p = 0; p < n; ++p) {
      auto extra = GameRegistry::instance().create_game(game_id_, seed_);
      ai_trackers_[p] = std::move(extra.belief_tracker);
      ai_encoders_[p] = std::move(extra.encoder);

      ai_views_[p] = bundle_->state->clone_state();
      if (bundle_->initial_observation_extractor &&
          bundle_->initial_observation_applier) {
        AnyMap obs = bundle_->initial_observation_extractor(*bundle_->state, p);
        bundle_->initial_observation_applier(*ai_views_[p], p, obs);
      }
      if (ai_trackers_[p]) {
        tracker_init(*ai_trackers_[p], *bundle_, *ai_views_[p], p);
      }
      if (!model_path_.empty() && ai_encoders_[p]) {
        ai_evaluators_[p] = std::make_unique<infer::OnnxPolicyValueEvaluator>(
            model_path_, ai_encoders_[p].get());
        if (!ai_evaluators_[p]->is_ready()) {
          throw std::runtime_error(
              "GameSession: failed to load model (ai_view[" + std::to_string(p) +
              "]): " + ai_evaluators_[p]->last_error());
        }
      }
    }
  }

  // Advance ai_view for a single perspective using the public-event
  // protocol: extract events from the truth transition for this observer,
  // then apply (pre-events → action → post-events) on ai_views_[p]. This
  // mirrors what the external AI API would see through apply_observation.
  void advance_ai_view_(int perspective, const IGameState& truth_before,
                        ActionId action) {
    if (perspective < 0 || perspective >= static_cast<int>(ai_views_.size())) return;
    if (!ai_views_[perspective]) return;
    const int actor = truth_before.current_player();
    if (!bundle_->public_event_extractor || !bundle_->public_event_applier) {
      // Fully-public game: just replay the action on ai_view, tracker
      // gets empty events (it has no hidden info to track).
      bundle_->rules->do_action_fast(*ai_views_[perspective], action);
      if (ai_trackers_[perspective]) {
        ai_trackers_[perspective]->observe_public_event(actor, action, {}, {});
      }
      return;
    }
    PublicEventTrace trace = bundle_->public_event_extractor(
        truth_before, action, *bundle_->state, perspective);
    for (const auto& [kind, payload] : trace.pre_events) {
      bundle_->public_event_applier(
          *ai_views_[perspective], EventPhase::kPreAction, kind, payload);
    }
    bundle_->rules->do_action_fast(*ai_views_[perspective], action);
    for (const auto& [kind, payload] : trace.post_events) {
      bundle_->public_event_applier(
          *ai_views_[perspective], EventPhase::kPostAction, kind, payload);
    }
    if (ai_trackers_[perspective]) {
      ai_trackers_[perspective]->observe_public_event(
          actor, action, trace.pre_events, trace.post_events);
    }
  }

  bool is_terminal() const { return bundle_->state->is_terminal(); }
  bool is_turn_start() const { return bundle_->state->is_turn_start(); }
  int current_player() const { return bundle_->state->current_player(); }
  int winner() const { return bundle_->state->winner(); }
  int num_players() const { return bundle_->state->num_players(); }
  std::string game_id() const { return game_id_; }

  py::dict get_state_dict() {
    if (!bundle_->state_serializer) {
      throw std::runtime_error("get_state_dict: game '" + game_id_ + "' has no state_serializer registered");
    }
    py::gil_scoped_release release;
    auto m = bundle_->state_serializer(*bundle_->state);
    py::gil_scoped_acquire acquire;
    py::dict out;
    for (const auto& [k, v] : m) out[py::cast(k)] = any_to_py(v);
    return out;
  }

  py::dict get_action_info(ActionId action) {
    if (!bundle_->action_descriptor) {
      throw std::runtime_error("get_action_info: game '" + game_id_ + "' has no action_descriptor registered");
    }
    auto m = bundle_->action_descriptor(action);
    py::dict out;
    for (const auto& [k, v] : m) out[py::cast(k)] = any_to_py(v);
    return out;
  }

  std::vector<ActionId> get_legal_actions() {
    py::gil_scoped_release release;
    const IGameRules& rules = filtered_rules_ ? *filtered_rules_ : *bundle_->rules;
    return rules.legal_actions(*bundle_->state);
  }

  std::vector<ActionId> get_all_legal_actions() {
    py::gil_scoped_release release;
    return bundle_->rules->legal_actions(*bundle_->state);
  }

  void apply_action(ActionId action) {
    py::gil_scoped_release release;
    const int actor = bundle_->state->current_player();
    std::unique_ptr<IGameState> state_before = bundle_->state->clone_state();
    bundle_->rules->do_action_fast(*bundle_->state, action);
    if (bt_) {
      tracker_observe(*bt_, *bundle_, *state_before, action, *bundle_->state,
                      actor);
    }

    // Advance each perspective's AI view via the public-event protocol.
    if (!external_obs_mode_) {
      const int n = static_cast<int>(ai_views_.size());
      for (int p = 0; p < n; ++p) {
        advance_ai_view_(p, *state_before, action);
      }
    }
    ++ply_count_;
  }

  // Combined action + events step for the AI API. Sequence:
  //   1. Snapshot actor = current_player (before the action)
  //   2. Apply all pre-action events (hidden info the action depends on)
  //   3. Apply the action itself
  //   4. Apply all post-action events (override random outcomes)
  //   5. belief_tracker.observe_public_event(actor, action, pre, post)
  //
  // Tracker is fed the event payloads directly — no state ref crosses its
  // interface. Pre/post lists are the same events the extractor would have
  // produced on a selfplay state diff, so tracker behavior matches across
  // selfplay and API paths (enforced by test_api_belief_matches_selfplay).
  //
  // `pre_events` / `post_events` are lists of {"kind": str, "payload": dict}.
  void apply_observation(ActionId action,
                         py::list pre_events,
                         py::list post_events) {
    if (!bundle_->public_event_applier) {
      throw std::runtime_error(
          "apply_observation: game '" + game_id_ +
          "' has no public_event_applier registered");
    }
    // Convert Python events to (kind, AnyMap) pairs BEFORE releasing GIL.
    std::vector<std::pair<std::string, AnyMap>> pre_list, post_list;
    auto convert = [](py::list src, std::vector<std::pair<std::string, AnyMap>>& dst) {
      for (py::handle item : src) {
        py::dict d = py::cast<py::dict>(item);
        std::string kind = py::cast<std::string>(d["kind"]);
        AnyMap payload = py_dict_to_any_map(py::cast<py::dict>(d["payload"]));
        dst.emplace_back(std::move(kind), std::move(payload));
      }
    };
    convert(pre_events, pre_list);
    convert(post_events, post_list);

    py::gil_scoped_release release;
    external_obs_mode_ = true;
    const int actor = bundle_->state->current_player();
    for (const auto& [kind, payload] : pre_list) {
      bundle_->public_event_applier(*bundle_->state, EventPhase::kPreAction, kind, payload);
    }
    bundle_->rules->do_action_fast(*bundle_->state, action);
    for (const auto& [kind, payload] : post_list) {
      bundle_->public_event_applier(*bundle_->state, EventPhase::kPostAction, kind, payload);
    }
    if (bt_) {
      // In API mode, the caller provides the event stream directly — feed
      // it straight to the tracker. No state-diff extraction needed.
      std::vector<PublicEvent> pre_events_v(pre_list.begin(), pre_list.end());
      std::vector<PublicEvent> post_events_v(post_list.begin(), post_list.end());
      bt_->observe_public_event(actor, action, pre_events_v, post_events_v);
      // Give the tracker a chance to reconcile public state invariants
      // that per-event appliers can't maintain (e.g. Splendor deck size
      // drifts when slot-shift and deck-draw are conflated in deck_flip
      // events). Default implementation is a no-op.
      bt_->reconcile_state(*bundle_->state);
    }
    ++ply_count_;
  }

  // Public-event protocol (used by the AI API). Applies an event to the
  // internal game state. `phase` is "pre" or "post" relative to an action;
  // the caller is responsible for ordering pre events BEFORE apply_action
  // and post events AFTER. The game's registered applier decides what
  // fields to mutate. Throws if the game did not register an applier.
  //
  // Prefer apply_observation() for the API driving use case — it handles
  // the action + events + observe sequencing atomically. apply_event is
  // kept for tests and debugging that want to drive the pieces separately.
  void apply_event(const std::string& phase, const std::string& kind, py::dict payload) {
    if (!bundle_->public_event_applier) {
      throw std::runtime_error(
          "apply_event: game '" + game_id_ + "' has no public_event_applier registered");
    }
    EventPhase ph;
    if (phase == "pre") ph = EventPhase::kPreAction;
    else if (phase == "post") ph = EventPhase::kPostAction;
    else throw std::invalid_argument("apply_event: phase must be 'pre' or 'post', got '" + phase + "'");
    AnyMap payload_map = py_dict_to_any_map(payload);
    py::gil_scoped_release release;
    // Switch into external observation mode: caller now drives the state
    // via the public-event protocol, so bundle_->state IS the AI view.
    // ai_views_ (set up in the constructor) are discarded as stale.
    external_obs_mode_ = true;
    bundle_->public_event_applier(*bundle_->state, ph, kind, payload_map);
  }

  // Partner-provided initial observation: perspective-specific info the AI
  // would know at game start (e.g. own starting hand). Overrides the
  // session's seed-generated hidden initial state for the perspective
  // player. Throws if the game registered no applier.
  void apply_initial_observation(int perspective_player, py::dict initial_obs) {
    if (!bundle_->initial_observation_applier) {
      throw std::runtime_error(
          "apply_initial_observation: game '" + game_id_ +
          "' has no initial_observation_applier registered");
    }
    AnyMap obs_map = py_dict_to_any_map(initial_obs);
    py::gil_scoped_release release;
    external_obs_mode_ = true;
    bundle_->initial_observation_applier(*bundle_->state, perspective_player, obs_map);
    if (bt_) bt_->init(perspective_player, obs_map);
  }

  // Return the belief tracker's serialized state as a dict. Canonical form:
  // two trackers with semantically identical beliefs return equal dicts.
  // Empty dict if no tracker or tracker holds no explicit state.
  py::dict get_belief_snapshot() {
    py::dict out;
    if (!bt_) return out;
    AnyMap m = bt_->serialize();
    for (const auto& [k, v] : m) out[py::cast(k)] = any_to_py(v);
    return out;
  }

  void configure_tail_solve(bool enabled, int depth_limit, std::int64_t node_budget) {
    ts_enabled_ = enabled;
    ts_depth_limit_ = depth_limit;
    ts_node_budget_ = node_budget;
  }

  py::dict get_ai_action(int simulations, double temperature) {
    py::gil_scoped_release release;

    const IGameRules& rules = filtered_rules_ ? *filtered_rules_ : *bundle_->rules;
    const int cp = bundle_->state->current_player();

    // Select the search state / evaluator / tracker.
    // External-obs mode (AI API): bundle_->state IS the AI view.
    // GameSession mode: route through perspective-specific ai_views_[cp].
    const IGameState* search_state = nullptr;
    const search::IPolicyValueEvaluator* eval_ptr = nullptr;
    IBeliefTracker* search_bt = nullptr;
    if (external_obs_mode_) {
      search_state = bundle_->state.get();
      eval_ptr = evaluator_.get();
      search_bt = bt_;
    } else {
      if (cp < 0 || cp >= static_cast<int>(ai_views_.size()) || !ai_views_[cp]) {
        throw std::runtime_error(
            "GameSession.get_ai_action: ai_view for current player not initialized");
      }
      search_state = ai_views_[cp].get();
      eval_ptr = ai_evaluators_[cp].get();
      search_bt = ai_trackers_[cp].get();
    }

    const auto legal = rules.legal_actions(*search_state);
    if (legal.empty()) {
      py::gil_scoped_acquire acquire;
      return py::dict();
    }

    if (!eval_ptr) {
      throw std::runtime_error(
          "GameSession.get_ai_action: no model loaded — create session with model_path");
    }

    // ISMCTS: root-sampling hidden info + DAG per-acting-player keying.
    search::NetMctsConfig mcts_cfg{};
    mcts_cfg.simulations = simulations;
    mcts_cfg.c_puct = 1.4f;
    if (search_bt) {
      mcts_cfg.root_belief_tracker = search_bt;
    }

    if (ts_enabled_ && bundle_->tail_solver) {
      int ply = static_cast<int>(ply_count_);
      bool try_ts = false;
      if (bundle_->tail_solve_trigger) {
        try_ts = bundle_->tail_solve_trigger(*search_state, ply);
      } else {
        try_ts = true;
      }
      if (try_ts) {
        mcts_cfg.tail_solve_enabled = true;
        mcts_cfg.tail_solve_config.depth_limit = ts_depth_limit_;
        mcts_cfg.tail_solve_config.node_budget = ts_node_budget_;
        mcts_cfg.tail_solver = bundle_->tail_solver.get();
      }
    }

    search::NetMcts mcts(mcts_cfg);
    search::NetMctsStats stats{};
    const std::uint64_t mcts_seed = seed_ ^
        (static_cast<std::uint64_t>(ply_count_) * kGoldenRatio64) ^ 0x243F6A8885A308D3ULL;
    mcts.search_root(*search_state, rules, *bundle_->value_model,
                      *eval_ptr, &stats, mcts_seed);

    // Deterministic action_seed: any non-deterministic component (e.g.
    // wall-clock) here would make argmax tie-breaks flaky across runs
    // even at temperature=0. Derive from (seed_, ply_count_) instead so
    // the session's action sequence is fully reproducible from its seed.
    std::uint64_t action_seed = mcts_seed ^ 0xBF58476D1CE4E5B9ULL;
    ActionId chosen = search::select_action_from_visits(
        stats.root_actions, stats.root_action_visits,
        temperature, action_seed, legal[0]);

    py::gil_scoped_acquire acquire;
    py::dict out;
    out["action"] = chosen;
    out["action_info"] = get_action_info(chosen);
    py::dict st;
    st["simulations"] = stats.simulations_done;
    st["best_value"] = stats.best_action_value;
    py::list rv;
    for (double v : stats.root_values) rv.append(v);
    st["root_values"] = rv;
    py::dict wm;
    for (size_t ei = 0; ei < stats.root_actions.size(); ++ei) {
      if (ei < stats.root_edge_values.size()) {
        py::list vals;
        for (double v : stats.root_edge_values[ei]) vals.append(v);
        wm[py::cast(static_cast<int>(stats.root_actions[ei]))] = vals;
      }
    }
    st["action_values"] = wm;
    // Expose root visit distribution so tests can compare the full
    // MCTS policy across paths (selfplay vs API), not just argmax.
    // This is the signal that would catch a subtle info leak biasing
    // one path's priors without flipping the top pick.
    py::list root_actions_py;
    py::list root_visits_py;
    for (size_t ei = 0; ei < stats.root_actions.size(); ++ei) {
      root_actions_py.append(static_cast<int>(stats.root_actions[ei]));
      int v = ei < stats.root_action_visits.size() ? stats.root_action_visits[ei] : 0;
      root_visits_py.append(v);
    }
    st["root_actions"] = root_actions_py;
    st["root_action_visits"] = root_visits_py;
    st["tail_solved"] = stats.tail_solved;
    st["tail_solve_value"] = stats.tail_solve_value;
    st["dag_reuse_hits"] = stats.dag_reuse_hits;
    st["expanded_nodes"] = stats.expanded_nodes;
    st["simulations"] = stats.simulations_done;
    out["stats"] = st;
    return out;
  }

  py::dict get_heuristic_action() {
    if (!bundle_->heuristic_picker) {
      throw std::runtime_error("get_heuristic_action: game '" + game_id_ + "' has no heuristic_picker registered");
    }

    ActionId chosen;
    {
      py::gil_scoped_release release;
      auto hr = bundle_->heuristic_picker(
          *bundle_->state, *bundle_->rules, seed_ ^ 0xDEAD);
      if (hr.actions.empty()) {
        py::gil_scoped_acquire acquire;
        return py::dict();
      }

      double max_score = *std::max_element(hr.scores.begin(), hr.scores.end());
      chosen = hr.actions[0];
      for (size_t i = 0; i < hr.scores.size(); ++i) {
        if (hr.scores[i] >= max_score - 1e-9) {
          chosen = hr.actions[i];
          break;
        }
      }
    }

    py::dict out;
    out["action"] = chosen;
    out["action_info"] = get_action_info(chosen);
    return out;
  }

  py::dict apply_ai_action(int simulations, double temperature) {
    auto result = get_ai_action(simulations, temperature);
    if (result.contains("action")) {
      apply_action(py::cast<ActionId>(result["action"]));
    }
    return result;
  }

 private:
  std::string game_id_;
  std::uint64_t seed_;
  std::string model_path_;
  std::unique_ptr<GameBundle> bundle_;
  std::unique_ptr<infer::OnnxPolicyValueEvaluator> evaluator_;
  std::unique_ptr<runtime::FilteredRulesWrapper> filtered_rules_;
  IBeliefTracker* bt_ = nullptr;
  std::size_t ply_count_ = 0;
  bool ts_enabled_ = false;
  int ts_depth_limit_ = 10;
  std::int64_t ts_node_budget_ = 200000;

  // Per-perspective AI views. Populated by the constructor via the
  // initial-observation protocol (unless external_obs_mode_ flips on
  // first — see comments at init_ai_views_). get_ai_action routes MCTS
  // through these so searches never read hidden fields from bundle_->state.
  std::vector<std::unique_ptr<IGameState>> ai_views_;
  std::vector<std::unique_ptr<IBeliefTracker>> ai_trackers_;
  std::vector<std::unique_ptr<IFeatureEncoder>> ai_encoders_;
  std::vector<std::unique_ptr<infer::OnnxPolicyValueEvaluator>> ai_evaluators_;
  bool external_obs_mode_ = false;
};

py::dict test_belief_tracker_py(
    const std::string& game_id,
    std::uint64_t seed,
    int plies,
    int randomize_trials) {
  py::gil_scoped_release release;

  auto bundle = GameRegistry::instance().create_game(game_id, seed);
  if (!bundle.belief_tracker) {
    py::gil_scoped_acquire acquire;
    throw std::runtime_error("test_belief_tracker: game must have belief_tracker");
  }

  auto state = bundle.state->clone_state();
  IBeliefTracker* bt = bundle.belief_tracker.get();
  tracker_init(*bt, bundle, *state, 0);

  std::mt19937_64 rng(seed);
  int actual_plies = 0;
  for (int i = 0; i < plies && !state->is_terminal(); ++i) {
    auto legal = bundle.rules->legal_actions(*state);
    if (legal.empty()) break;
    const size_t idx = rng() % legal.size();
    ActionId chosen = legal[idx];
    auto state_before = state->clone_state();
    bundle.rules->do_action_fast(*state, chosen);
    tracker_observe(*bt, bundle, *state_before, chosen, *state, 0);
    actual_plies += 1;
  }

  auto extract_deck_ids = [&](const IGameState& gs) -> std::vector<int> {
    if (!bundle.state_serializer) return {};
    auto m = bundle.state_serializer(gs);
    std::vector<int> result;
    auto it = m.find("_test_all_deck_ids");
    if (it != m.end()) {
      result = std::any_cast<std::vector<int>>(it->second);
    }
    return result;
  };

  auto extract_tableau_ids = [&](const IGameState& gs) -> std::vector<int> {
    if (!bundle.state_serializer) return {};
    auto m = bundle.state_serializer(gs);
    std::vector<int> result;
    auto it = m.find("_test_tableau_ids");
    if (it != m.end()) {
      result = std::any_cast<std::vector<int>>(it->second);
    }
    return result;
  };

  std::vector<int> original_deck = extract_deck_ids(*state);
  std::vector<int> tableau_ids = extract_tableau_ids(*state);

  std::vector<std::vector<int>> trial_decks;
  trial_decks.reserve(static_cast<size_t>(randomize_trials));
  for (int t = 0; t < randomize_trials; ++t) {
    auto clone = state->clone_state();
    std::mt19937 trial_rng(static_cast<unsigned>(seed ^ static_cast<std::uint64_t>(t + 1)));
    bt->randomize_unseen(*clone, trial_rng);
    trial_decks.push_back(extract_deck_ids(*clone));
  }

  py::gil_scoped_acquire acquire;
  py::dict out;
  out["plies"] = actual_plies;
  out["original_deck"] = original_deck;
  out["tableau_cards"] = tableau_ids;
  py::list trials;
  for (const auto& d : trial_decks) {
    trials.append(py::cast(d));
  }
  out["trial_decks"] = trials;
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
      py::arg("max_game_plies") = 500,
      py::arg("tail_solve_enabled") = false,
      py::arg("tail_solve_start_ply") = 40,
      py::arg("tail_solve_depth_limit") = 5,
      py::arg("tail_solve_node_budget") = 10000000LL,
      py::arg("tail_solve_margin_weight") = 0.0f,
      py::arg("temperature_initial") = -1.0,
      py::arg("temperature_final") = -1.0,
      py::arg("temperature_decay_plies") = 0,
      py::arg("heuristic_guidance_ratio") = 0.0,
      py::arg("heuristic_temperature") = 0.0,
      py::arg("training_filter_ratio") = 1.0,
      py::arg("nopeek_enabled") = true,
      py::arg("trace_perspective") = -1);

  m.def("run_arena_match", &run_arena_match_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("model_paths"),
      py::arg("simulations_list"),
      py::arg("temperature") = 0.0,
      py::arg("max_game_plies") = 500,
      py::arg("tail_solve") = false);

  m.def("run_constrained_eval_vs_heuristic", &run_constrained_eval_vs_heuristic_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("model_path"),
      py::arg("simulations") = 200,
      py::arg("model_is_player") = 0,
      py::arg("constrained") = true,
      py::arg("heuristic_temperature") = 0.0);

  m.def("run_heuristic_episode", &run_heuristic_episode_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("temperature") = 0.0,
      py::arg("max_game_plies") = 200);

  m.def("encode_state", &encode_state_py,
      py::arg("game_id"),
      py::arg("seed") = 0xC0FFEE);

  m.def("tail_solve", &tail_solve_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("perspective_player") = 0,
      py::arg("depth_limit") = 10,
      py::arg("node_budget") = 200000LL);

  m.def("test_belief_tracker", &test_belief_tracker_py,
      py::arg("game_id"),
      py::arg("seed"),
      py::arg("plies") = 20,
      py::arg("randomize_trials") = 10);

  m.def("game_metadata", [](const std::string& game_id) -> py::dict {
    py::gil_scoped_release release;
    auto bundle = GameRegistry::instance().create_game(game_id, 0);
    const int num_players = bundle.state->num_players();
    const int action_space = bundle.encoder->action_space();
    const int feature_dim = bundle.encoder->feature_dim();
    py::gil_scoped_acquire acquire;
    py::dict out;
    out["num_players"] = num_players;
    out["action_space"] = action_space;
    out["feature_dim"] = feature_dim;
    return out;
  }, py::arg("game_id"));

  m.def("available_games", []() -> std::vector<std::string> {
    py::gil_scoped_release release;
    return GameRegistry::instance().game_ids();
  });

  py::class_<GameSessionWrapper>(m, "GameSession")
      .def(py::init<const std::string&, std::uint64_t, const std::string&, bool>(),
           py::arg("game_id"),
           py::arg("seed") = 0xC0FFEE,
           py::arg("model_path") = "",
           py::arg("use_filter") = false)
      .def_property_readonly("is_terminal", &GameSessionWrapper::is_terminal)
      .def_property_readonly("is_turn_start", &GameSessionWrapper::is_turn_start)
      .def_property_readonly("current_player", &GameSessionWrapper::current_player)
      .def_property_readonly("winner", &GameSessionWrapper::winner)
      .def_property_readonly("num_players", &GameSessionWrapper::num_players)
      .def_property_readonly("game_id", &GameSessionWrapper::game_id)
      .def("get_state_dict", &GameSessionWrapper::get_state_dict)
      .def("get_action_info", &GameSessionWrapper::get_action_info)
      .def("get_legal_actions", &GameSessionWrapper::get_legal_actions)
      .def("get_all_legal_actions", &GameSessionWrapper::get_all_legal_actions)
      .def("apply_action", &GameSessionWrapper::apply_action)
      .def("apply_observation", &GameSessionWrapper::apply_observation,
           py::arg("action"),
           py::arg("pre_events") = py::list(),
           py::arg("post_events") = py::list())
      .def("apply_event", &GameSessionWrapper::apply_event,
           py::arg("phase"), py::arg("kind"), py::arg("payload"))
      .def("apply_initial_observation", &GameSessionWrapper::apply_initial_observation,
           py::arg("perspective_player"), py::arg("initial_observation"))
      .def("get_belief_snapshot", &GameSessionWrapper::get_belief_snapshot)
      .def("get_ai_action", &GameSessionWrapper::get_ai_action,
           py::arg("simulations") = 200,
           py::arg("temperature") = 0.0)
      .def("get_heuristic_action", &GameSessionWrapper::get_heuristic_action)
      .def("configure_tail_solve", &GameSessionWrapper::configure_tail_solve,
           py::arg("enabled"),
           py::arg("depth_limit") = 10,
           py::arg("node_budget") = 200000LL)
      .def("apply_ai_action", &GameSessionWrapper::apply_ai_action,
           py::arg("simulations") = 200,
           py::arg("temperature") = 0.0);
}
