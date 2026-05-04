#include "selfplay_runner.h"

#include <cassert>
#include <cmath>
#include <numeric>
#include <random>

namespace board_ai::runtime {

SelfplayEpisodeResult run_selfplay_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const search::IPolicyValueEvaluator& evaluator,
    const SelfplayConfig& config,
    std::uint64_t episode_seed,
    IBeliefTracker* belief_tracker,
    const IFeatureEncoder* encoder,
    const search::ITailSolver* tail_solver,
    GameAdjudicator adjudicator,
    AuxiliaryScorer auxiliary_scorer,
    HeuristicPicker heuristic_picker,
    TrainingActionFilter training_action_filter,
    TailSolveTrigger tail_solve_trigger,
    EpisodeStatsExtractor episode_stats_extractor,
    int trace_perspective,
    IBeliefTracker* trace_belief_tracker,
    PublicEventExtractor public_event_extractor,
    InitialObservationExtractor initial_observation_extractor) {
  SelfplayEpisodeResult result{};

  const bool tracing =
      trace_perspective >= 0 && public_event_extractor && trace_belief_tracker;
  if (tracing) {
    result.trace_enabled = true;
    result.trace_perspective = trace_perspective;
  }

  std::unique_ptr<FilteredRulesWrapper> filtered_rules_ptr;
  if (training_action_filter) {
    filtered_rules_ptr = std::make_unique<FilteredRulesWrapper>(rules, training_action_filter);
  }

  auto state = initial_state.clone_state();
  int ply = 0;

  // Capture initial observation + belief for the traced perspective.
  // trace_belief_tracker is a SEPARATE instance dedicated to this
  // perspective (primary belief_tracker gets re-init'd every ply for MCTS).
  if (tracing) {
    AnyMap trace_init_obs;
    if (initial_observation_extractor) {
      trace_init_obs = initial_observation_extractor(*state, trace_perspective);
    }
    trace_belief_tracker->init(trace_perspective, trace_init_obs);
    result.initial_belief_snapshot = trace_belief_tracker->serialize();
    result.initial_observation = trace_init_obs;
  }

  std::mt19937 heuristic_rng(static_cast<unsigned>(episode_seed ^ 0xDEADBEEF));
  std::uniform_real_distribution<double> heuristic_dist(0.0, 1.0);
  int heuristic_moves = 0;

  while (!state->is_terminal() && ply < config.max_game_plies) {
    const int player = state->current_player();

    const bool use_filter = filtered_rules_ptr &&
        config.training_filter_ratio > 0.0 &&
        heuristic_dist(heuristic_rng) < config.training_filter_ratio;
    const IGameRules& effective_rules = use_filter ? *filtered_rules_ptr : rules;

    const auto legal = effective_rules.legal_actions(*state);
    if (legal.empty()) break;
    const auto& full_legal = use_filter ? rules.legal_actions(*state) : legal;

    const bool use_heuristic = heuristic_picker &&
        config.heuristic_guidance_ratio > 0.0 &&
        heuristic_dist(heuristic_rng) < config.heuristic_guidance_ratio;

    if (use_heuristic) {
      const std::uint64_t hr_seed = episode_seed ^ (static_cast<std::uint64_t>(ply) * kGoldenRatio64);
      auto hr = heuristic_picker(*state, effective_rules, hr_seed);

      const size_t n_actions = hr.actions.size();
      std::vector<double> probs(n_actions);
      double max_score = *std::max_element(hr.scores.begin(), hr.scores.end());

      if (config.heuristic_temperature > 1e-6) {
        double inv_temp = 1.0 / config.heuristic_temperature;
        double sum_exp = 0.0;
        for (size_t i = 0; i < n_actions; ++i) {
          probs[i] = std::exp((hr.scores[i] - max_score) * inv_temp);
          sum_exp += probs[i];
        }
        for (auto& p : probs) p /= sum_exp;
      } else {
        int count = 0;
        for (size_t i = 0; i < n_actions; ++i) {
          if (hr.scores[i] >= max_score - 1e-9) ++count;
        }
        for (size_t i = 0; i < n_actions; ++i) {
          probs[i] = (hr.scores[i] >= max_score - 1e-9) ? 1.0 / count : 0.0;
        }
      }

      // Convert to fake visits for policy target
      std::vector<int> fake_visits(n_actions);
      for (size_t i = 0; i < n_actions; ++i) {
        fake_visits[i] = static_cast<int>(probs[i] * 10000.0 + 0.5);
      }

      // Sample action from distribution
      std::uint64_t h = hr_seed;
      h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
      h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
      h ^= h >> 31;
      double r = static_cast<double>(h & 0xFFFFFFFF) / 4294967296.0;
      size_t chosen_idx = n_actions - 1;
      double cumulative = 0.0;
      for (size_t i = 0; i < n_actions; ++i) {
        cumulative += probs[i];
        if (r <= cumulative) { chosen_idx = i; break; }
      }
      ActionId chosen = hr.actions[chosen_idx];

      SelfplaySample sample{};
      sample.ply = ply;
      sample.player = player;
      sample.action_id = chosen;
      sample.policy_action_ids = hr.actions;
      if (encoder) {
        encoder->encode(*state, player, full_legal, &sample.features, &sample.legal_mask);
      }
      sample.policy_action_visits = fake_visits;
      if (auxiliary_scorer) {
        sample.auxiliary_score = auxiliary_scorer(*state, player);
        sample.has_auxiliary_score = true;
      }
      result.samples.push_back(std::move(sample));

      std::unique_ptr<IGameState> state_before;
      if (belief_tracker || tracing) state_before = state->clone_state();
      effective_rules.do_action_fast(*state, chosen);
      if (belief_tracker) {
        PublicEventTrace evt;
        if (public_event_extractor) {
          evt = public_event_extractor(*state_before, chosen, *state, player);
        }
        belief_tracker->observe_public_event(
            player, chosen, evt.pre_events, evt.post_events);
      }
      if (tracing) {
        SelfplayObservationTrace t{};
        t.ply = ply;
        t.actor = player;
        t.action = chosen;
        auto evt = public_event_extractor(
            *state_before, chosen, *state, trace_perspective);
        t.pre_events = evt.pre_events;
        t.post_events = evt.post_events;
        trace_belief_tracker->observe_public_event(
            player, chosen, evt.pre_events, evt.post_events);
        t.belief_snapshot_after = trace_belief_tracker->serialize();
        result.observation_trace.push_back(std::move(t));
      }
      ply += 1;
      heuristic_moves += 1;
      continue;
    }

    if (belief_tracker) {
      AnyMap main_init_obs;
      if (initial_observation_extractor) {
        main_init_obs = initial_observation_extractor(*state, player);
      }
      belief_tracker->init(player, main_init_obs);
    }

    const auto noise = search::resolve_root_dirichlet_noise(
        config.dirichlet_alpha, config.dirichlet_epsilon,
        config.dirichlet_on_first_n_plies, ply);

    const bool try_tail_solve = config.tail_solve_enabled && tail_solver &&
        (tail_solve_trigger ? tail_solve_trigger(*state, ply) : ply >= config.tail_solve_start_ply);

    search::NetMctsConfig mcts_cfg{};
    mcts_cfg.simulations = config.simulations;
    mcts_cfg.c_puct = config.c_puct;
    mcts_cfg.max_depth = config.max_depth;
    mcts_cfg.value_clip = config.value_clip;
    mcts_cfg.root_dirichlet_alpha = noise.alpha;
    mcts_cfg.root_dirichlet_epsilon = noise.epsilon;
    // ISMCTS: root-sampling hidden info + DAG per-acting-player keying.
    // MCTS uses the per-sim sampled world's rules.legal_actions at each node.
    if (belief_tracker) {
      mcts_cfg.root_belief_tracker = belief_tracker;
    }

    if (try_tail_solve) {
      mcts_cfg.tail_solve_enabled = true;
      mcts_cfg.tail_solve_config = config.tail_solve_config;
      if (auxiliary_scorer && config.tail_solve_config.margin_weight != 0.0f &&
          !config.tail_solve_config.margin_scorer) {
        mcts_cfg.tail_solve_config.margin_scorer = auxiliary_scorer;
      }
      mcts_cfg.tail_solver = tail_solver;
    }

    search::NetMcts mcts(mcts_cfg);
    search::NetMctsStats stats{};
    const std::uint64_t mcts_seed = episode_seed ^
        (static_cast<std::uint64_t>(ply) * kGoldenRatio64) ^ 0x243F6A8885A308D3ULL;
    mcts.search_root(*state, effective_rules, value_model, evaluator, &stats, mcts_seed);

    if (stats.tail_solve_attempted) {
      result.tail_solve_attempts += 1;
      result.tail_solve_total_ms += stats.tail_solve_elapsed_ms;
      if (stats.tail_solve_completed) {
        result.tail_solve_completed += 1;
      }
      if (stats.tail_solved) {
        result.tail_solve_successes += 1;
      }
    }

    SelfplaySample sample{};
    sample.ply = ply;
    sample.player = player;
    sample.policy_action_ids = stats.root_actions;
    sample.policy_action_visits = stats.root_action_visits;
    sample.tail_solved = stats.tail_solved;
    if (encoder) {
      encoder->encode(*state, player, full_legal, &sample.features, &sample.legal_mask);
    }
    if (auxiliary_scorer) {
      sample.auxiliary_score = auxiliary_scorer(*state, player);
      sample.has_auxiliary_score = true;
    }

    const double temperature = stats.tail_solved ? 0.0 :
        search::resolve_linear_temperature(config.temperature_schedule, config.temperature, ply);
    const std::uint64_t action_seed = episode_seed ^ (static_cast<std::uint64_t>(ply) * kGoldenRatio64);
    const ActionId chosen = search::select_action_from_visits(
        stats.root_actions, stats.root_action_visits, temperature, action_seed, legal[0]);

    sample.action_id = chosen;
    result.samples.push_back(std::move(sample));

    std::unique_ptr<IGameState> state_before;
    if (belief_tracker || tracing) state_before = state->clone_state();
    effective_rules.do_action_fast(*state, chosen);
    if (belief_tracker) {
      PublicEventTrace evt;
      if (public_event_extractor) {
        evt = public_event_extractor(*state_before, chosen, *state, player);
      }
      belief_tracker->observe_public_event(
          player, chosen, evt.pre_events, evt.post_events);
    }
    if (tracing) {
      SelfplayObservationTrace t{};
      t.ply = ply;
      t.actor = player;
      t.action = chosen;
      auto evt = public_event_extractor(
          *state_before, chosen, *state, trace_perspective);
      t.pre_events = evt.pre_events;
      t.post_events = evt.post_events;
      trace_belief_tracker->observe_public_event(
          player, chosen, evt.pre_events, evt.post_events);
      t.belief_snapshot_after = trace_belief_tracker->serialize();
      result.observation_trace.push_back(std::move(t));
    }
    ply += 1;
  }

  result.total_plies = ply;
  result.final_state = state->clone_state();

  if (state->is_terminal()) {
    const int num_players = state->num_players();
    float best_val = -2.0f;
    int best_player = -1;
    bool is_draw = true;
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (v > best_val) {
        best_val = v;
        best_player = p;
      }
    }
    for (int p = 0; p < num_players; ++p) {
      const float v = value_model.terminal_value_for_player(*state, p);
      if (std::abs(v - best_val) > 1e-6f) {
        is_draw = false;
        break;
      }
    }
    if (is_draw) {
      result.draw = true;
      result.winner = -1;
    } else {
      result.draw = false;
      result.winner = best_player;
    }

    const auto terminal_vals = value_model.terminal_values(*state);
    assert(std::abs(std::accumulate(terminal_vals.begin(), terminal_vals.end(), 0.0f)) < 1e-4f);
    for (auto& s : result.samples) {
      s.z_values = terminal_vals;
      if (result.draw) {
        s.z = 0.0f;
      } else {
        s.z = (s.player == result.winner) ? 1.0f : -1.0f;
      }
    }
  } else if (adjudicator) {
    const int adj_winner = adjudicator(*state);
    if (adj_winner < 0) {
      result.draw = true;
      result.winner = -1;
    } else {
      result.draw = false;
      result.winner = adj_winner;
    }
    const int num_players = state->num_players();
    std::vector<float> adj_vals(num_players, 0.0f);
    if (!result.draw) {
      const float loser_val = -1.0f / static_cast<float>(num_players - 1);
      for (int p = 0; p < num_players; ++p) {
        adj_vals[p] = (p == result.winner) ? 1.0f : loser_val;
      }
    }
    assert(std::abs(std::accumulate(adj_vals.begin(), adj_vals.end(), 0.0f)) < 1e-4f);
    for (auto& s : result.samples) {
      s.z_values = adj_vals;
      if (result.draw) {
        s.z = 0.0f;
      } else {
        s.z = (s.player == result.winner) ? 1.0f : -1.0f;
      }
    }
  } else {
    // Game didn't finish and no adjudicator — treat as draw (all zeros).
    const int np = state->num_players();
    std::vector<float> zero_vals(np, 0.0f);
    for (auto& s : result.samples) {
      s.z_values = zero_vals;
      s.z = 0.0f;
    }
    result.draw = true;
    result.winner = -1;
  }

  if (episode_stats_extractor) {
    std::vector<SelfplaySampleView> views;
    views.reserve(result.samples.size());
    for (const auto& s : result.samples) {
      views.push_back({s.ply, s.player, s.action_id});
    }
    result.custom_stats = episode_stats_extractor(*state, views);
  }

  return result;
}

}  // namespace board_ai::runtime
