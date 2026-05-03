#include "heuristic_runner.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace board_ai::runtime {

std::size_t sample_heuristic_index(
    const std::vector<double>& scores,
    double temperature,
    double rng_u01) {
  if (scores.empty()) return 0;
  const double max_score = *std::max_element(scores.begin(), scores.end());
  if (temperature <= 1e-6) {
    // Uniform random over all argmax ties (contract documented in the
    // development guide; selfplay_runner does the same). Do NOT return the
    // first max_element — for games where many actions share the top score
    // (e.g. Quoridor, where every pawn-advance and every effective wall both
    // score +1), that degenerates to "always pick the first action in the
    // legal-action ordering" and makes the heuristic deterministic.
    std::size_t count = 0;
    for (std::size_t i = 0; i < scores.size(); ++i) {
      if (scores[i] >= max_score - 1e-9) ++count;
    }
    if (count <= 1) {
      auto it = std::max_element(scores.begin(), scores.end());
      return static_cast<std::size_t>(it - scores.begin());
    }
    std::size_t pick = static_cast<std::size_t>(rng_u01 * static_cast<double>(count));
    if (pick >= count) pick = count - 1;
    std::size_t seen = 0;
    for (std::size_t i = 0; i < scores.size(); ++i) {
      if (scores[i] >= max_score - 1e-9) {
        if (seen == pick) return i;
        ++seen;
      }
    }
    return scores.size() - 1;
  }
  const double inv_temp = 1.0 / temperature;
  std::vector<double> probs(scores.size());
  double sum_exp = 0.0;
  for (std::size_t i = 0; i < scores.size(); ++i) {
    probs[i] = std::exp((scores[i] - max_score) * inv_temp);
    sum_exp += probs[i];
  }
  if (sum_exp <= 0.0) return 0;
  const double r = rng_u01 * sum_exp;
  double cumulative = 0.0;
  for (std::size_t i = 0; i < probs.size(); ++i) {
    cumulative += probs[i];
    if (r <= cumulative) return i;
  }
  return probs.size() - 1;
}

SelfplayEpisodeResult run_heuristic_episode(
    IGameState& initial_state,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const IFeatureEncoder* encoder,
    const HeuristicPicker& heuristic,
    double temperature,
    int max_game_plies,
    std::uint64_t episode_seed,
    AuxiliaryScorer auxiliary_scorer,
    GameAdjudicator adjudicator) {
  SelfplayEpisodeResult result{};

  auto state = initial_state.clone_state();
  int ply = 0;

  std::uint64_t rng = episode_seed;
  auto next_rng = [&rng]() -> std::uint64_t {
    rng += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = rng;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  };

  while (!state->is_terminal() && ply < max_game_plies) {
    const int player = state->current_player();
    auto legal = rules.legal_actions(*state);
    if (legal.empty()) break;

    std::vector<float> features;
    std::vector<float> legal_mask;
    if (encoder) {
      encoder->encode(*state, player, legal, &features, &legal_mask);
    }

    HeuristicResult hr = heuristic(*state, rules, next_rng());

    if (hr.scores.empty() || hr.actions.empty()) {
      throw std::runtime_error(
          "run_heuristic_episode: heuristic_picker returned empty result at ply " +
          std::to_string(ply));
    }

    double rng_u01 = static_cast<double>(next_rng() & 0xFFFFFFFF) / 4294967296.0;
    size_t chosen_idx = sample_heuristic_index(hr.scores, temperature, rng_u01);
    ActionId chosen = hr.actions[chosen_idx];

    // Convert scores to fake visits for pipeline compatibility
    std::vector<int> fake_visits(hr.scores.size());
    if (temperature <= 1e-6) {
      double max_sc = *std::max_element(hr.scores.begin(), hr.scores.end());
      for (size_t i = 0; i < hr.scores.size(); ++i) {
        fake_visits[i] = (hr.scores[i] >= max_sc - 1e-9) ? 1 : 0;
      }
    } else {
      double max_sc = *std::max_element(hr.scores.begin(), hr.scores.end());
      double inv_t = 1.0 / temperature;
      double sum_exp = 0.0;
      std::vector<double> probs(hr.scores.size());
      for (size_t i = 0; i < hr.scores.size(); ++i) {
        probs[i] = std::exp((hr.scores[i] - max_sc) * inv_t);
        sum_exp += probs[i];
      }
      for (size_t i = 0; i < probs.size(); ++i) {
        fake_visits[i] = static_cast<int>(probs[i] / sum_exp * 10000.0 + 0.5);
      }
    }

    SelfplaySample sample{};
    sample.ply = ply;
    sample.player = player;
    sample.action_id = chosen;
    sample.policy_action_ids = hr.actions;
    sample.policy_action_visits = fake_visits;
    sample.features = features;
    sample.legal_mask = legal_mask;
    if (auxiliary_scorer) {
      sample.auxiliary_score = auxiliary_scorer(*state, player);
      sample.has_auxiliary_score = true;
    }
    result.samples.push_back(std::move(sample));

    rules.do_action_fast(*state, chosen);
    ++ply;
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

    auto terminal_vals = value_model.terminal_values(*state);
    for (auto& s : result.samples) {
      s.z_values = terminal_vals;
      s.z = result.draw ? 0.0f : ((s.player == result.winner) ? 1.0f : -1.0f);
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
    const int np = state->num_players();
    std::vector<float> adj_vals(np, 0.0f);
    if (!result.draw) {
      const float loser_val = -1.0f / static_cast<float>(np - 1);
      for (int p = 0; p < np; ++p) {
        adj_vals[p] = (p == result.winner) ? 1.0f : loser_val;
      }
    }
    assert(std::abs(std::accumulate(adj_vals.begin(), adj_vals.end(), 0.0f)) < 1e-4f);
    for (auto& s : result.samples) {
      s.z_values = adj_vals;
      s.z = result.draw ? 0.0f : ((s.player == result.winner) ? 1.0f : -1.0f);
    }
  } else {
    for (auto& s : result.samples) {
      s.z = 0.0f;
      s.z_values.assign(static_cast<size_t>(state->num_players()), 0.0f);
    }
  }

  return result;
}

}  // namespace board_ai::runtime
