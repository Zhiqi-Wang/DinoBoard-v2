#include "net_mcts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace board_ai::search {

namespace {

class SplitMix64Engine {
 public:
  using result_type = std::uint64_t;

  explicit SplitMix64Engine(std::uint64_t seed) : state_(seed) {}

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() { return UINT64_MAX; }

  result_type operator()() {
    return splitmix64(state_);
  }

 private:
  std::uint64_t state_;
};

struct Edge {
  ActionId action = -1;
  float prior = 0.0f;
  int child = -1;           // index into `nodes` vector; -1 if not yet traversed
  int visit_count = 0;
  float value_sum = 0.0f;
};

struct Node {
  int to_play = 0;
  bool expanded = false;
  int visit_count = 0;
  float value_sum = 0.0f;
  std::vector<Edge> edges{};
};

static float clip_value(float v, float lim) {
  const float cap = std::max(0.0f, lim);
  return std::max(-cap, std::min(cap, v));
}

static void apply_root_dirichlet_noise(Node& root, float alpha, float epsilon, std::uint64_t seed) {
  if (root.edges.empty()) return;
  const float a = std::max(0.0f, alpha);
  const float eps = std::max(0.0f, std::min(1.0f, epsilon));
  if (a <= 1e-8f || eps <= 1e-8f) return;
  SplitMix64Engine rng(seed);
  std::gamma_distribution<float> gamma(a, 1.0f);
  std::vector<float> noise(root.edges.size(), 0.0f);
  float sum = 0.0f;
  for (size_t i = 0; i < root.edges.size(); ++i) {
    const float g = std::max(0.0f, gamma(rng));
    noise[i] = g;
    sum += g;
  }
  if (sum <= 1e-12f) return;
  for (size_t i = 0; i < root.edges.size(); ++i) {
    const float n = noise[i] / sum;
    const float p = std::max(0.0f, root.edges[i].prior);
    root.edges[i].prior = (1.0f - eps) * p + eps * n;
  }
}

}  // namespace

NetMcts::NetMcts(NetMctsConfig cfg) : cfg_(cfg) {}

ActionId select_action_from_visits(
    const std::vector<ActionId>& actions,
    const std::vector<int>& visits,
    double temperature,
    std::uint64_t rng_seed,
    ActionId fallback_action) {
  if (actions.empty() || actions.size() != visits.size()) return fallback_action;
  if (temperature <= 1e-6) {
    int best_visit = std::numeric_limits<int>::min();
    std::vector<size_t> best_indices;
    best_indices.reserve(actions.size());
    for (size_t i = 0; i < visits.size(); ++i) {
      if (visits[i] > best_visit) {
        best_visit = visits[i];
        best_indices.clear();
        best_indices.push_back(i);
      } else if (visits[i] == best_visit) {
        best_indices.push_back(i);
      }
    }
    if (best_indices.empty()) return fallback_action;
    if (best_indices.size() == 1) return actions[best_indices.front()];
    SplitMix64Engine rng(rng_seed);
    std::uniform_int_distribution<size_t> pick(0, best_indices.size() - 1);
    return actions[best_indices[pick(rng)]];
  }

  const double inv_t = 1.0 / std::max(1e-6, temperature);
  std::vector<double> weights(actions.size(), 0.0);
  double sum_w = 0.0;
  for (size_t i = 0; i < visits.size(); ++i) {
    const double base = static_cast<double>(std::max(0, visits[i]));
    const double w = (base > 0.0) ? std::pow(base, inv_t) : 0.0;
    weights[i] = std::isfinite(w) ? w : 0.0;
    sum_w += weights[i];
  }
  if (sum_w <= 1e-12) return fallback_action;

  SplitMix64Engine rng(rng_seed);
  std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
  return actions[dist(rng)];
}

// ISMCTS-v2 search.
//
// Algorithm (single simulation):
//   1. Clone root. If belief_tracker set, call randomize_unseen to sample
//      a belief-consistent hidden world (root determinization). Descent is
//      now deterministic within this sampled world.
//   2. Descend from root, using UCT2 for edge selection:
//        score = Q + c_puct * prior * sqrt(last_edge.visits) / (1 + edge.visits)
//      where last_edge is the edge we came through into the current node.
//      At root, last_edge.visits is approximated by root_node.visit_count
//      (equals sim count so far).
//   3. At each node, compute the current state's hash using
//      state.state_hash_for_perspective(state.current_player()). Look up
//      in global `node_index` table. If found, reuse (DAG share). If not,
//      create new node.
//   4. On reaching an unexpanded (leaf) node, call evaluator.evaluate(
//      sim_state, current_player, legal_actions) to get priors + values.
//      Store priors on edges (one per legal action; no full_action_space).
//   5. Backup leaf_values up the path: node.visit++, edge.visit++ per step.
//
// Invariants:
//   - DAG is acyclic (guaranteed by state.step_count monotonicity in hash).
//   - Nodes at the same (public, acting-player-private, step) are shared,
//     giving info-set statistics aggregation across sampled worlds.
//   - No chance node / NoPeek / perturb_rng machinery.
ActionId NetMcts::search_root(
    const IGameState& root,
    const IGameRules& rules,
    const IStateValueModel& value_model,
    const IPolicyValueEvaluator& evaluator,
    NetMctsStats* stats,
    std::uint64_t seed) const {
  const auto legal_root = rules.legal_actions(root);
  if (legal_root.empty()) {
    if (stats) *stats = {};
    return -1;
  }

  if (cfg_.tail_solve_enabled && cfg_.tail_solver) {
    auto solve_state = root.clone_state();
    const TailSolveResult ts = cfg_.tail_solver->solve(
        *solve_state, rules, value_model, root.current_player(), cfg_.tail_solve_config);
    if (stats) {
      stats->tail_solve_attempted = true;
      stats->tail_solve_completed = !ts.budget_exceeded;
      stats->tail_solve_elapsed_ms = ts.elapsed_ms;
    }
    if (ts.outcome == TailSolveOutcome::kProvenWin && ts.best_action >= 0) {
      if (stats) {
        stats->tail_solved = true;
        stats->tail_solve_outcome = ts.outcome;
        stats->tail_solve_value = ts.value;
        stats->simulations_done = 0;
        stats->root_actions = legal_root;
        stats->root_action_visits.assign(legal_root.size(), 0);
        for (size_t i = 0; i < legal_root.size(); ++i) {
          if (legal_root[i] == ts.best_action) {
            stats->root_action_visits[i] = 1;
            break;
          }
        }
        stats->best_action_value = static_cast<double>(ts.value);
      }
      return ts.best_action;
    }
  }

  const auto t0 = std::chrono::steady_clock::now();

  std::vector<Node> nodes;
  nodes.reserve(static_cast<size_t>(std::max(512, cfg_.simulations * 2)));

  // Global hash → node_index table: the DAG's canonical lookup. Cleared per
  // search_root call (cross-call sharing is not attempted; the step_count in
  // hash would separate states from different sessions anyway).
  std::unordered_map<StateHash64, int> node_index;

  // Compute node key from a sampled world's current state, using the
  // acting player's information set.
  auto compute_hash = [](const IGameState& s) -> StateHash64 {
    return s.state_hash_for_perspective(s.current_player());
  };

  const StateHash64 root_hash = compute_hash(root);
  nodes.push_back(Node{root.current_player(), false, 0, 0.0f, {}});
  node_index[root_hash] = 0;

  auto expand_node = [&](Node& node, const IGameState& state) -> std::vector<float> {
    const auto legal = rules.legal_actions(state);
    if (legal.empty()) {
      node.expanded = true;
      node.edges.clear();
      return value_model.terminal_values(state);
    }

    std::vector<float> priors;
    std::vector<float> values;
    const bool ok = evaluator.evaluate(state, node.to_play, legal, &priors, &values);
    if (!ok) {
      throw std::runtime_error("MCTS: evaluator.evaluate() failed — model not loaded or inference error");
    }
    if (priors.size() != legal.size()) {
      throw std::runtime_error("MCTS: evaluator returned " + std::to_string(priors.size()) +
          " priors but " + std::to_string(legal.size()) + " actions");
    }

    float sum = 0.0f;
    for (float p : priors) sum += std::max(0.0f, p);
    if (sum <= 1e-8f) {
      const float uniform = 1.0f / static_cast<float>(legal.size());
      priors.assign(legal.size(), uniform);
    } else {
      for (float& p : priors) p = std::max(0.0f, p) / sum;
    }

    node.edges.clear();
    node.edges.reserve(legal.size());
    for (size_t i = 0; i < legal.size(); ++i) {
      Edge edge{};
      edge.action = legal[i];
      edge.prior = priors[i];
      node.edges.push_back(std::move(edge));
    }
    node.expanded = true;
    return values;
  };

  (void)expand_node(nodes[0], root);
  const std::uint64_t dirichlet_seed = (seed != 0)
      ? (seed ^ 0xA076178CDFB1AC2DULL ^
         static_cast<std::uint64_t>(root.current_player() + 13))
      : (static_cast<std::uint64_t>(t0.time_since_epoch().count()) ^
         static_cast<std::uint64_t>(root.current_player() + 13));
  apply_root_dirichlet_noise(
      nodes[0],
      cfg_.root_dirichlet_alpha,
      cfg_.root_dirichlet_epsilon,
      dirichlet_seed);

  const int np = root.num_players();
  std::vector<std::vector<double>> root_edge_values(
      nodes[0].edges.size(), std::vector<double>(static_cast<size_t>(np), 0.0));

  const int simulations = std::max(1, cfg_.simulations);

  // Per-sim RNG for root determinization. Each sim picks a distinct world.
  std::mt19937 root_sample_rng(
      seed ^ 0x51ED270FABCDEF01ULL ^ static_cast<std::uint64_t>(simulations));

  std::int64_t dag_reuse_hits = 0;

  for (int sim = 0; sim < simulations; ++sim) {
    std::unique_ptr<IGameState> sim_state = root.clone_state();
    if (cfg_.root_belief_tracker != nullptr) {
      std::mt19937 per_sim_rng(static_cast<std::uint32_t>(root_sample_rng()));
      cfg_.root_belief_tracker->randomize_unseen(*sim_state, per_sim_rng);
    }

    // Path records for backup. For UCT2 we also track which edge we came
    // through INTO each node on the path; the sqrt() in UCB uses that edge's
    // visit_count, not the node's global visit_count (which in a DAG mixes
    // visits from multiple incoming paths).
    std::vector<int> path_nodes;
    std::vector<int> path_edges;     // edge index within path_nodes[i]
    path_nodes.reserve(static_cast<size_t>(cfg_.max_depth + 2));
    path_edges.reserve(static_cast<size_t>(cfg_.max_depth + 2));

    int cur_idx = 0;
    path_nodes.push_back(cur_idx);

    std::vector<float> leaf_values;
    int depth = 0;

    // At root, there's no incoming edge; use root's node visit_count as the
    // sqrt argument (equals sim count so far).
    int incoming_edge_visits = nodes[0].visit_count;

    while (depth < cfg_.max_depth) {
      if (sim_state->is_terminal()) {
        leaf_values = value_model.terminal_values(*sim_state);
        break;
      }
      if (!nodes[cur_idx].expanded) {
        leaf_values = expand_node(nodes[cur_idx], *sim_state);
        break;
      }
      if (nodes[cur_idx].edges.empty()) {
        leaf_values = value_model.terminal_values(*sim_state);
        break;
      }

      // UCT2 edge selection. sqrt() argument is the visit count of the edge
      // we just came through (incoming_edge_visits), NOT the node's global
      // visit_count. Avoids over-exploration bias from DAG's multiple
      // parents (Childs et al. 2008).
      const float sqrt_parent = std::sqrt(
          static_cast<float>(std::max(1, incoming_edge_visits)));
      int best_edge = -1;
      float best_score = -std::numeric_limits<float>::infinity();
      for (int ei = 0; ei < static_cast<int>(nodes[cur_idx].edges.size()); ++ei) {
        const Edge& e = nodes[cur_idx].edges[ei];
        float q = 0.0f;
        if (e.visit_count > 0) q = e.value_sum / static_cast<float>(e.visit_count);
        const float u = cfg_.c_puct * e.prior * sqrt_parent /
                        (1.0f + static_cast<float>(e.visit_count));
        const float score = q + u;
        if (score > best_score) {
          best_score = score;
          best_edge = ei;
        }
      }
      if (best_edge < 0) {
        leaf_values = value_model.terminal_values(*sim_state);
        break;
      }

      const ActionId chosen_action = nodes[cur_idx].edges[best_edge].action;
      // Defensive legality check. In DAG MCTS, two simulations can reach the
      // same (hash) DAG node with technically-different states if the hash
      // function misses a field that affects legal_actions (a hash-scope
      // incompleteness bug). Rather than crash, we re-filter this node's
      // edges against the current state's actual legal set and re-pick from
      // those. This is a workaround — the real fix is to make hash_public +
      // hash_private(cur_player) fully determine legal_actions for every
      // game. For games where it does (which SHOULD be every game), this
      // fallback never triggers.
      if (!rules.validate_action(*sim_state, chosen_action)) {
        auto current_legal = rules.legal_actions(*sim_state);
        std::unordered_set<ActionId> legal_set(current_legal.begin(),
                                               current_legal.end());
        // Re-select the best edge restricted to currently-legal actions.
        int fallback_edge = -1;
        float fallback_score = -std::numeric_limits<float>::infinity();
        const float fb_sqrt_parent = std::sqrt(static_cast<float>(
            std::max(1, incoming_edge_visits)));
        for (int ei = 0; ei < static_cast<int>(nodes[cur_idx].edges.size()); ++ei) {
          const Edge& e = nodes[cur_idx].edges[ei];
          if (!legal_set.count(e.action)) continue;
          float q = 0.0f;
          if (e.visit_count > 0) q = e.value_sum / static_cast<float>(e.visit_count);
          const float u = cfg_.c_puct * e.prior * fb_sqrt_parent /
                          (1.0f + static_cast<float>(e.visit_count));
          const float score = q + u;
          if (score > fallback_score) {
            fallback_score = score;
            fallback_edge = ei;
          }
        }
        if (fallback_edge < 0) {
          // No overlap between node's edges and current legal set: the state
          // at this node truly doesn't share a legal-action set. Terminate
          // this simulation at leaf — use the node's value estimate.
          leaf_values = value_model.terminal_values(*sim_state);
          break;
        }
        best_edge = fallback_edge;
      }
      const ActionId final_action = nodes[cur_idx].edges[best_edge].action;
      rules.do_action_fast(*sim_state, final_action);

      // DAG node lookup: after do_action, compute hash under the NEW
      // current_player's perspective (decision node = acting-player view).
      const StateHash64 next_hash = compute_hash(*sim_state);
      int next_idx = -1;
      auto it = node_index.find(next_hash);
      if (it != node_index.end()) {
        next_idx = it->second;
        ++dag_reuse_hits;
      } else {
        nodes.push_back(Node{sim_state->current_player(), false, 0, 0.0f, {}});
        next_idx = static_cast<int>(nodes.size()) - 1;
        node_index[next_hash] = next_idx;
      }

      // Update this edge's `child` field to the resolved target. Since the
      // DAG may have this edge point to different children on different
      // paths (e.g. if the rules-apply leads to different worlds), we always
      // overwrite — but for correctness we expect the same (edge, sim world)
      // to produce the same hash, so this is typically stable.
      nodes[cur_idx].edges[best_edge].child = next_idx;

      path_edges.push_back(best_edge);
      cur_idx = next_idx;
      path_nodes.push_back(cur_idx);
      // For next iteration's UCT2: the "incoming edge" is the one we just
      // traversed.
      incoming_edge_visits = nodes[path_nodes[path_nodes.size() - 2]]
                                 .edges[best_edge]
                                 .visit_count;
      depth += 1;
    }

    // Backup. Standard path-walk; in a DAG each node's visit_count tracks
    // total visits across all parent paths.
    for (int i = static_cast<int>(path_nodes.size()) - 1; i >= 0; --i) {
      const int node_idx = path_nodes[static_cast<size_t>(i)];
      Node& n = nodes[node_idx];
      const size_t tp = static_cast<size_t>(n.to_play);
      const float v = (tp < leaf_values.size())
          ? clip_value(leaf_values[tp], cfg_.value_clip) : 0.0f;
      n.visit_count += 1;
      n.value_sum += v;
      if (i > 0) {
        const int parent_idx = path_nodes[static_cast<size_t>(i - 1)];
        const int parent_edge_idx = path_edges[static_cast<size_t>(i - 1)];
        const size_t parent_tp = static_cast<size_t>(nodes[parent_idx].to_play);
        const float pv = (parent_tp < leaf_values.size())
            ? clip_value(leaf_values[parent_tp], cfg_.value_clip) : 0.0f;
        Edge& parent_edge = nodes[parent_idx].edges[parent_edge_idx];
        parent_edge.visit_count += 1;
        parent_edge.value_sum += pv;
        if (parent_idx == 0) {
          auto& rev = root_edge_values[static_cast<size_t>(parent_edge_idx)];
          for (int p = 0; p < np; ++p) {
            const double cv = (static_cast<size_t>(p) < leaf_values.size())
                ? static_cast<double>(clip_value(leaf_values[static_cast<size_t>(p)], cfg_.value_clip))
                : 0.0;
            rev[static_cast<size_t>(p)] += cv;
          }
        }
      }
    }
  }

  const Node& root_node = nodes[0];
  // Tie-break random selection when multiple edges share max visit count.
  int best_edge = 0;
  int best_visit = -1;
  std::vector<int> tied_edges;
  for (int ei = 0; ei < static_cast<int>(root_node.edges.size()); ++ei) {
    const int vc = root_node.edges[ei].visit_count;
    if (vc > best_visit) {
      best_visit = vc;
      tied_edges.clear();
      tied_edges.push_back(ei);
    } else if (vc == best_visit) {
      tied_edges.push_back(ei);
    }
  }
  if (tied_edges.size() == 1) {
    best_edge = tied_edges.front();
  } else if (!tied_edges.empty()) {
    SplitMix64Engine tiebreak_rng(seed ^ 0xCC9E2D51FBF7B96DULL);
    std::uniform_int_distribution<size_t> pick(0, tied_edges.size() - 1);
    best_edge = tied_edges[pick(tiebreak_rng)];
  }

  if (stats) {
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::max(1e-9, std::chrono::duration<double>(t1 - t0).count());
    stats->simulations_done = simulations;
    stats->expanded_nodes = static_cast<std::int64_t>(nodes.size());
    stats->nodes_per_sec = static_cast<double>(nodes.size()) / sec;
    stats->dag_reuse_hits = dag_reuse_hits;
    stats->root_actions.clear();
    stats->root_action_visits.clear();
    stats->root_actions.reserve(root_node.edges.size());
    stats->root_action_visits.reserve(root_node.edges.size());
    for (const Edge& e : root_node.edges) {
      stats->root_actions.push_back(e.action);
      stats->root_action_visits.push_back(e.visit_count);
    }
    const Edge& best = root_node.edges[best_edge];
    float q = 0.0f;
    if (best.visit_count > 0) q = best.value_sum / static_cast<float>(best.visit_count);
    stats->best_action_value = static_cast<double>(clip_value(q, cfg_.value_clip));
    stats->root_values.resize(static_cast<size_t>(np));
    if (best.visit_count > 0) {
      const auto& rev = root_edge_values[static_cast<size_t>(best_edge)];
      for (int p = 0; p < np; ++p) {
        stats->root_values[static_cast<size_t>(p)] = rev[static_cast<size_t>(p)] / best.visit_count;
      }
    }
    stats->root_edge_values.resize(root_node.edges.size());
    for (size_t ei = 0; ei < root_node.edges.size(); ++ei) {
      auto& out = stats->root_edge_values[ei];
      out.resize(static_cast<size_t>(np), 0.0);
      const int vc = root_node.edges[ei].visit_count;
      if (vc > 0) {
        const auto& rev = root_edge_values[ei];
        for (int p = 0; p < np; ++p) {
          out[static_cast<size_t>(p)] = rev[static_cast<size_t>(p)] / vc;
        }
      }
    }
  }
  return root_node.edges[best_edge].action;
}

}  // namespace board_ai::search
