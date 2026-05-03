# DinoBoard v2 Design Principles

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

- The entire AI decision pipeline — belief tracking, feature encoding, MCTS search — must work solely from the observation history (action sequence + publicly visible events). It must NEVER depend on reading the true game state's hidden fields.
- Rationale: the engine must support an API mode where an external game server runs the ground truth and the AI only receives observations ("player X played action Y"). There is no game state object to read — only what a real player sitting at the table would know.
- This principle applies to every component in the AI chain:
  - **Belief tracker**: maintains its own model of what is known/unknown, updated solely through `observe_action()`. `randomize_unseen()` reconstructs the unseen pool from accumulated observations, not from the true state.
  - **Feature encoder**: when encoding other players' hidden information (e.g., opponent's face-down reserved cards), must encode what the current perspective player *knows* (via belief tracker or public info), not what the state object contains.
  - **MCTS search**: operates on worlds constructed by `randomize_unseen`, which are belief-consistent hypotheticals, not the true game state.
- This applies to ALL games: Splendor deck contents, Azul bag contents, opponent hands, or any future game's hidden information. The belief tracker is the player's memory; the game state is the server's truth.
- **Validation is mandatory, not optional**. Code review alone cannot prove separation — a single `checked_cast` followed by a hidden-field read could slip through unnoticed. The ONLY way to certify that a game's AI pipeline respects this principle is by passing the two-layer AI API test:
  - `tests/test_ai_api_separation.py::test_full_game_via_api[<game>]` — API contract carries no state fields in or out.
  - `tests/test_api_belief_matches_selfplay.py::*[<game>]` (random games only) — AI session initialized with a DIFFERENT seed than ground truth must produce the same belief / public state / legal actions after replaying the observation stream. If the AI secretly reads ground truth's state anywhere, at least one of these three assertions diverges.
- A new game isn't accepted until these pass. See `docs/GAME_DEVELOPMENT_GUIDE.md §17` for the event protocol and acceptance workflow.

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
- **docs/KNOWN_ISSUES.md** — bug postmortems and design trade-off records. BUG-001 through BUG-020 covers every shipped regression: tail solver TT flags, draw z-value, train-eval action space mismatch, FilteredRulesWrapper const_cast, replay buffer utilization, feature encoding pipeline bug, Splendor temperature schedule, replay buffer loss, ONNX silent degradation, model export order, z_values incomplete, legal mask filter, belief tracker peeking, adjudicator z_values + 3p+ evaluator, 2p-hardcoded multiplayer paths, pipeline stats-key mismatch. Plus general pitfalls and design decisions. Read this before writing new game logic or modifying the pipeline.
- **docs/NEW_GAME_TEST_GUIDE.md** — step-by-step verification checklist for new game implementations. 9 steps: registration + config consistency, GameSession interaction, do/undo consistency, feature encoding (BUG-007 regression), selfplay sample integrity, ONNX round-trip, training tensor validation, optional component verification (heuristic, tail solver, filter, adjudicator, auxiliary scorer, hidden info), and multiplayer variants. Includes instructions for joining the existing 600+ parametrized test suite.
- **docs/devlog/YYYY-MM-DD.md** — daily development log. Record what was implemented, key decisions made, config changes, and training observations. Keep entries concise and factual.
