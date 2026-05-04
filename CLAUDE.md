# DinoBoard Design Principles

## Training Pipeline

- Training is config-driven: only `game.json` parameters, no code changes per training run.
- Self-play loss not decreasing is normal (opponent also gets stronger). Don't diagnose it as a problem.
- Latest model always moves forward, never rolls back. Best model is tracked independently.
- Eval must use the same compiled code as selfplay. If code changes between build and eval, results are meaningless.
- The entire selfplay pipeline (selfplay, eval, arena) runs in C++. Never write Python fallback or reimplementation of any game logic or search logic. If you find yourself writing Python code that "does what the C++ does but slower", stop — that's a bug. The correct fix is always to expose the needed C++ functionality through pybind11.

## No Fallbacks, No Silent Degradation

- Never write fallback logic that masks errors. If something fails, throw/raise immediately — don't return a default value that makes the caller think it succeeded.
- Every function that receives data from C++ must access fields with `dict["key"]`, not `dict.get("key", default)`. Missing fields are bugs; defaults hide them.
- ONNX model is mandatory for selfplay, arena, and eval. Empty `model_path` is always an error. There is no uniform-policy fallback.
- `evaluate()` failures must throw, never return false. A silent `return false` caused BUG-011 (weeks of wasted training).
- `catch (...) { return false; }` is banned. Re-throw with context so the caller sees the real error.
- If you're writing code that makes a broken system "still run", stop. The correct fix is to make the system not broken.

## AI Pipeline Independence from Game State

The AI decision pipeline — belief tracking, feature encoding, MCTS search — must work solely from observation history. It NEVER reads ground truth's hidden fields.

**Architecture**:

- **Ground truth** maintains game progress (our C++ engine, external API, or a physical tabletop game). Ground truth advances itself and sends messages to the AI. The AI advances its own dataset + tracker from these messages.
- **AI dataset** splits into **public fields** (all players see) and **private fields per player** (only that player sees). Declared via:
  ```cpp
  virtual void hash_public_fields(Hasher&) const = 0;
  virtual void hash_private_fields(int player, Hasher&) const = 0;
  ```
  Framework derives `state_hash_for_perspective(p) = hash(public + private_of_p)`. The same partition drives encoder feature extraction.
- **Tracker** maintains what a given perspective player has learned through legal observation. `init(perspective, initial_observation)` and `observe_public_event(actor, action, pre_events, post_events)` take NO `IGameState*` — the tracker physically cannot peek at truth.
- **MCTS** uses root determinization: each simulation begins with `tracker->randomize_unseen(sim_state, rng)` — tracker fills certainties (e.g. known opp hand from Priest peek), rest of unseen pool sampled per tracker's policy (uniform for simple cases; heuristic weighted for bluff-heavy games like Coup where claim/challenge history biases opp role priors), remainder shuffled into deck. After sampling, descent is fully deterministic within that world.
- **Node hashing in MCTS** uses `state_hash_for_perspective(state.current_player())` — each decision node is keyed by the acting player's information set. This forms a **DAG** (transpositions reached by different paths share nodes via a global `hash → node_index` table).
- **DAG acyclicity** is guaranteed structurally by `IGameState::step_count` — a framework-provided counter that increments on every `do_action_fast`, included in public hash. No two states in the DAG share a hash unless they have the same step count.
- **UCB** uses UCT2 (DAG-aware): the `sqrt(parent.visit_count)` term uses the visit count of the specific incoming edge traversed in this simulation, not the DAG node's global visit count. Avoids over-exploration bias from multi-parent aggregation.
- **Encoder**: extracts features only from `public_fields + current_player's private_fields`. The public/private partition is the single source of truth for both hash and encoder scope — they stay aligned structurally.

**Validation** (mandatory for every game, enforced in CI):

- `tests/test_ai_api_separation.py::test_full_game_via_api[<game>]` — the API carries no state fields in/out. The AI drives a full game from observations alone.
- `tests/test_api_belief_matches_selfplay.py::*[<game>]` (stochastic games) — AI session seeded differently from ground truth produces identical belief + public state + legal actions after the same observation stream.
- `tests/test_api_mcts_policy_invariance.py::*[<game>]` — MCTS visit distribution on the same observation history is identical across selfplay and API paths. If the AI secretly reads truth, distributions diverge.
- `tests/test_encoder_respects_hash_scope.py::*[<game>]` — encoder output is bit-equal when opp private changes but (public + own private) stays the same.
- `tests/test_dag_acyclic.py::*[<game>]` — no cycles in the MCTS DAG, asserted via step_count monotonicity.

**No chance nodes**: physical randomness (deck draws, dice) is handled entirely by root determinization — different simulations sample different worlds, and different observer-visible outcomes automatically produce different hashes → different DAG nodes. No special chance-node machinery (NoPeek / traversal limiter / `chance_outcomes` / afterstate cap / `stochastic_detector`) exists in the framework.

## Value Head / Multiplayer

- Value head outputs N-dimensional value (N = num_players), not scalar. Even 2-player games output 2 dims.
- Value output is perspective-relative, aligned with the encoder's feature rotation: `values[0]` = my value, `values[1]` = next player, etc.
- The ONNX evaluator rotates the N-dim output back to absolute player ordering for MCTS backup.
- `IStateValueModel::terminal_values()` must satisfy zero-sum: `sum(values) ≈ 0`. The network learns this from targets, not from architectural constraint.
- Training targets use `sample["z_values"]` (per-player vector) rotated to perspective order, not the scalar `sample["z"]`.
- `game_metadata(game_id)` returns `{num_players, action_space, feature_dim}` from C++ — always prefer this over hardcoded JSON values for variant-aware code.

## Game Architecture

- Engine is fully game-agnostic. All game-specific logic lives in the game's GameBundle registration.
- One game = state + rules + net_adapter + register + config/game.json + web frontend.

## Web Frontend Design

Full visual/interaction guide: `docs/WEB_DESIGN_PRINCIPLES.md`.

- Web frontend is mandatory. It is the primary interface for players to play and for the developer to verify training results.
- Actions must NOT be mapped naively to individual buttons. Design interactions as the player would naturally play the physical game.
- Spatial anchoring: fixed game regions must have fixed screen positions and sizes. Never let a container shrink or shift when its contents change.
- Every action (human or AI) must have animated transitions via `describeTransition`. No instant state jumps.
- Visual clarity: sufficient color contrast, clear current-player indicator, hover feedback on interactive elements.
- Never hold a global lock (mutex, SQLite write lock, etc.) while running heavy computation (MCTS search, batch inference). Heavy work goes to a thread pool; locks are session-scoped and held only for state reads/writes.
- The Python GIL counts as a global lock. Any pybind11 binding that performs meaningful C++ work (MCTS, ONNX loading/inference, batch encoding) MUST wrap that work in `py::gil_scoped_release`. Without it, offloading the call to a ThreadPoolExecutor does nothing — the worker thread acquires the GIL for the entire C++ call and stalls every other Python thread, including the uvicorn request loop, causing the UI to freeze for seconds per move.

## Documentation

When implementing a new feature or fixing a bug, update documentation immediately:

- **README.md** — keep concise; only mention the feature exists, don't explain implementation details.
- **docs/GAME_FEATURES_OVERVIEW.md** — high-level "what's available" for developers. Training pipeline, search, decision-making, training enhancements, eval, web frontend, randomness handling, optional component reference, config reference, and new game development steps. Start here for a quick overview of what the framework can do.
- **docs/GAME_DEVELOPMENT_GUIDE.md** — detailed implementation guide. Covers IGameState, IGameRules, IFeatureEncoder, GameBundle registration, GameRegistrar patterns, game.json config format (all fields), CMake/setup.py build integration, all 12 optional components with signatures and examples, feature encoding best practices, and web frontend integration (createApp API, ctx/gameState objects, common.js utilities). This is the single source of truth for "how to add a new game."
- **docs/KNOWN_ISSUES.md** — bug postmortems and design trade-off records. BUG-001 through BUG-022 covers every shipped regression: tail solver TT flags, draw z-value, train-eval action space mismatch, FilteredRulesWrapper const_cast, replay buffer utilization, feature encoding pipeline bug, Splendor temperature schedule, replay buffer loss, ONNX silent degradation, model export order, z_values incomplete, legal mask filter, belief tracker peeking, adjudicator z_values + 3p+ evaluator, 2p-hardcoded multiplayer paths, pipeline stats-key mismatch, fly animation inheriting container size + sequential playback, cancel_pipeline side-effect wiping precompute cache. Plus general pitfalls and design decisions. Read this before writing new game logic or modifying the pipeline.
- **docs/NEW_GAME_TEST_GUIDE.md** — step-by-step verification checklist for new game implementations. 9 steps: registration + config consistency, GameSession interaction, do/undo consistency, feature encoding (BUG-007 regression), selfplay sample integrity, ONNX round-trip, training tensor validation, optional component verification (heuristic, tail solver, filter, adjudicator, auxiliary scorer, hidden info), and multiplayer variants. Includes instructions for joining the existing 600+ parametrized test suite.
- **docs/devlog/YYYY-MM-DD.md** — daily development log. Record what was implemented, key decisions made, config changes, and training observations. Keep entries concise and factual.
