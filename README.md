# DinoBoard

> **Drop a rulebook. Ship a superhuman AI.**
>
> Tell Claude Code "add Azul" in one sentence — it reads the rulebook, writes the C++, runs the full test suite, trains a superhuman AI, and generates the web frontend. You just sign off.

**中文版:** [README_CN.md](README_CN.md)

A general-purpose board-game AI engine — **one framework, one engineering investment, unlimited game reuse**. AlphaZero-style MCTS + neural self-play, with first-class support for 2–4 players, hidden information, and bluffing games.

---

## What this project solves

AI in digital board games is usually weak — not because the techniques don't exist, but because **rebuilding AlphaZero from scratch for every title is too costly**. MCTS, ONNX integration, training pipelines, hidden-information handling, tuning traps — every game re-walks the same road.

DinoBoard walks it once and turns the result into **a reusable engine plus a callable API**:

- **~9.4k lines of C++/Python core** — MCTS, belief tracker, training, web, analysis, all generic
- **~2000 lines to add a new game** — rules + feature encoder + JSON config; the framework owns the rest
- **Massive parametrized test coverage auto-applies** — a new game inherits all acceptance tests by adding it to `CANONICAL_GAMES`
- **6 games covering 4 paradigms** — perfect info, symmetric randomness, asymmetric hidden info, bluffing
- **Observation-only REST API for third parties** — digital board game apps / platforms / companion apps call the AI directly without sharing any game-state code or embedding the C++ engine
- **One code path from training to web to external API** — the same C++ MCTS serves self-play, live play, replay analysis, and third-party integration, with zero "training vs. production drift"

> Context: OpenSpiel has algorithms but no product surface (no web, no ONNX, no API). Commercial board-game apps ship AI that plays near randomly (Splendor / Azul / Coup apps are perennially criticized). This project fills the gap where **research-grade algorithms, production-grade engineering, and an out-of-the-box integration interface** all meet.

---

## Why it is technically strong

### ISMCTS: chance-node-free DAG search

A ground-up MCTS redesign for hidden-information games. **Root-sampling determinization + per-acting-player info-set keying + UCT2** — each simulation samples a full world from the belief, descent is fully deterministic afterward, and the same info set reached along different paths shares a DAG node. No NoPeek / traversal limiter / chance-outcome machinery required. See [docs/MCTS_ALGORITHM.md](docs/MCTS_ALGORITHM.md).

### Observation-only AI API: trained once, callable by anyone

The framework ships a REST API that third-party systems integrate against with **zero shared code**:

```
POST /ai/sessions                         → create an AI session
POST /ai/sessions/{id}/observe            → tell the AI what happened (action id + public events)
POST /ai/sessions/{id}/decide             → get the AI's chosen action
DELETE /ai/sessions/{id}                  → end the session
```

**The caller does not need the game-state code, the C++ engine, or any knowledge of MCTS.** As long as they can translate their own game events into action ids + public events, they can use a superhuman AI as a black-box opponent or coach.

This is not a stripped-down interface — it runs **the exact same MCTS + belief tracker + ONNX inference as self-play training**. The observation-only design is a structural constraint (the `IBeliefTracker::observe_public_event()` signature has no `IGameState*` parameter), which means:

- AI decisions depend only on observation history and can never peek at ground truth → **cheating is structurally impossible**
- The same AI serves self-play training, web play, and the third-party API → **one training investment, three deployment surfaces**
- Independent-seed belief-equivalence tests give an information-theoretic proof of the separation → **you can prove to a client that the AI does not cheat**

Fits: existing digital board game apps that want stronger AI opponents, platforms that want to offer AI coaches, physical tabletop companion apps that need live suggestions.

### Pluggable belief sampling: uniform → heuristic → neural

The same `randomize_unseen` interface supports three escalating strengths:

- **Uniform sampling** for simple stochastic games like Azul
- **Hand-crafted probabilistic heuristics** (Coup's sampler uses claim / challenge history to bias opponent-role priors, avoiding the "never-challenge-never-bluff" degenerate equilibrium)
- **Neural belief networks** (interface-ready; a sequence model can drop in to replace the heuristic)

This is one of the few places the framework was **designed specifically for imperfect-information games**. Most open-source AlphaZero projects handle perfect-info only.

### Training–inference parity

Self-play, evaluation, web play, replay analysis — **all run on the same C++ MCTS**. No "Python in training, rewritten C++ in serving" translation drift. When a player sees "this move dropped my win rate from 62% to 41%" on the web, that number is literally the value head's output during training.

### Engineering discipline

- Heavy parametrized testing; every new game inherits the full suite for free
- `docs/KNOWN_ISSUES.md` documents 22 shipped bugs and design trade-offs — **every pothole the next integrator gets to skip**
- Strict no-fallback discipline (see `CLAUDE.md`): silent degradation is banned, errors must propagate to the surface

---

## Games already shipped

| Game | Players | Challenge |
|------|---------|-----------|
| **TicTacToe** | 2 | Minimal template |
| **Quoridor** | 2 | Long horizon, highly strategic |
| **Splendor** | 2–4 | Symmetric randomness + blind reserved cards |
| **Azul** | 2–4 | Bag draws (symmetric physical randomness) |
| **Love Letter** | 2–4 | Asymmetric hidden info + player elimination + precise knowledge tracking |
| **Coup** | 2–4 | Bluffing + 11-phase state machine + heuristic belief |

Every game ships with a web frontend (animations, undo, smart hints, replay with per-move loss analysis).

---

## Adding a new game: one conversation

You no longer need to hand-write the game bundle. The typical flow:

1. Tell Claude Code "add [game name]"
2. The AI reads `docs/GAME_DEVELOPMENT_GUIDE.md` and `docs/KNOWN_ISSUES.md`, and mirrors Quoridor / Splendor / the other existing games
3. Add the new game to `tests/conftest.py::CANONICAL_GAMES` so the parametrized test suite runs against it automatically
4. The AI iterates on test failures until everything is green
5. `python -m training.cli --game <id>` kicks off training
6. Open the web UI to accept

**Your job shrinks to: one request + reviewing the PR + starting the training run.**

This flow works in practice because every integration point has a **mechanically verifiable contract** (tests + `KNOWN_ISSUES` pothole list), so the AI can close the loop by itself.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Python layer                     │
│  training/pipeline.py  ←→  bindings/py_engine.cpp   │
│  training/cli.py            (pybind11)              │
│  platform/app.py       ←→  GameSession              │
├─────────────────────────────────────────────────────┤
│                    C++ engine                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ runtime/ │  │ search/  │  │ infer/           │   │
│  │ selfplay │→ │ NetMCTS  │→ │ ONNX Evaluator   │   │
│  │ arena    │  │ (ISMCTS  │  │ (optional ONNX)  │   │
│  │ heuristic│  │  DAG)    │  │                  │   │
│  └──────────┘  │ TailSolv │  └──────────────────┘   │
│                └──────────┘                         │
│  ┌──────────────────────────────────────────────┐   │
│  │ core/ — interface definitions                │   │
│  │ IGameState · IGameRules · IFeatureEncoder    │   │
│  │ IBeliefTracker · GameRegistry · GameBundle   │   │
│  └──────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────┤
│                    Game implementations             │
│  games/tictactoe/  games/quoridor/                  │
│  games/splendor/   games/azul/                      │
│  games/loveletter/ games/coup/                      │
└─────────────────────────────────────────────────────┘
```

Full directory tree at the bottom of this file.

---

## Quick start

### Requirements

The core is C++; Python is glue. You need:

- **A C++17 compiler** — Mac: `xcode-select --install`; Linux: `apt install build-essential`; Windows: MSVC Build Tools
- **Python ≥ 3.9** with `pybind11` and `torch` (for training)
- **ONNX Runtime** — web play, self-play, and evaluation **all load `.onnx` models for the AI to move**; this is required, not optional. Mac: `brew install onnxruntime`. Linux / Windows: download the platform package from [ONNX Runtime releases](https://github.com/microsoft/onnxruntime/releases) and unpack it.

### Build

```bash
pip install pybind11 torch

# Standard build (Mac brew / Linux system paths detect ONNX Runtime automatically)
pip install -e .

# If ONNX Runtime is not in a standard path, point at it explicitly
BOARD_AI_WITH_ONNX=1 \
  BOARD_AI_ONNXRUNTIME_ROOT=/path/to/onnxruntime \
  pip install -e .

# Verify
python -c "import dinoboard_engine; print(dinoboard_engine.available_games())"
# ['azul', 'azul_2p', ..., 'quoridor', 'splendor', ..., 'tictactoe']
```

> `setup.py` prints a warning and keeps building if ONNX Runtime is missing — that path exists only so basic tests can run. **Web play and training will both fail later because the model cannot load.**

### Training

```bash
# TicTacToe — about 5 minutes
python -m training.cli --game tictactoe --output runs/tictactoe_001

# Quoridor — several hours (includes warm start + heuristic guidance)
python -m training.cli --game quoridor --output runs/quoridor_001 \
    --workers 4 --eval-every 25 --eval-games 40 --eval-benchmark heuristic
```

Training is driven by `games/<game>/config/game.json` — no code changes, just JSON. See the [features overview § config quick reference](docs/GAME_FEATURES_OVERVIEW.md).

### Web play

```bash
pip install -r requirements.txt  # fastapi, uvicorn
cd platform && python -m uvicorn app:app --host 0.0.0.0 --port 8000
open http://localhost:8000
```

Features: 6 games, three difficulty tiers (Heuristic / Casual / Expert), seat selection for multi-player variants, undo, smart hints, replay with per-move loss analysis.

---

## Core concepts

- **GameBundle registration** — each game exposes a factory that returns state + rules + encoder + optional components. See the [game development guide](docs/GAME_DEVELOPMENT_GUIDE.md).
- **ISMCTS** — root sampling + DAG + UCT2. A native design for hidden-info games, no chance-node machinery needed. See [docs/MCTS_ALGORITHM.md](docs/MCTS_ALGORITHM.md).
- **AI API** — observation-only REST interface that third-party apps consume directly, without embedding engine code. Doubles as an information-theoretic proof that the AI never cheats. See [game development guide § 17](docs/GAME_DEVELOPMENT_GUIDE.md).
- **Training pipeline** — self-play → replay buffer → SGD → ONNX export → gating eval (≥60% win rate updates `best`). Includes warm start, heuristic guidance, auxiliary score, training action filter, MCTS schedule.

---

## Docs

- **[Features overview](docs/GAME_FEATURES_OVERVIEW.md)** — what the framework can do
- **[Game development guide](docs/GAME_DEVELOPMENT_GUIDE.md)** — single source of truth for adding a new game
- **[MCTS algorithm](docs/MCTS_ALGORITHM.md)** — the ISMCTS DAG-search derivation
- **[New game test guide](docs/NEW_GAME_TEST_GUIDE.md)** — 9-step acceptance workflow
- **[Known issues & trade-offs](docs/KNOWN_ISSUES.md)** — BUG-001 through BUG-022 postmortems plus design decisions

---

## Collaboration

This project targets **research-grade board-game AI engineering** and **digital board-game AI consulting**. If you need:

- A stronger AI opponent or AI coach for a digital board game
- Observation-only AI inference integrated into your gaming platform

open an Issue or Discussion.

---

## Directory layout

```
DinoBoard/
├── engine/                         # C++ general engine
│   ├── core/                       # Interface definitions
│   │   ├── game_interfaces.h       #   IGameState, IGameRules, IStateValueModel
│   │   ├── feature_encoder.h       #   IFeatureEncoder
│   │   ├── belief_tracker.h        #   IBeliefTracker (hidden info)
│   │   ├── game_registry.h         #   GameBundle, GameRegistrar
│   │   ├── types.h                 #   ActionId, StateHash64, UndoToken
│   │   └── action_constraint.h     #   IActionConstraint
│   ├── search/                     # Search algorithms
│   │   ├── net_mcts.h/.cpp         #   PUCT-MCTS + neural evaluation
│   │   ├── tail_solver.h/.cpp      #   Alpha-beta endgame solver
│   │   ├── root_noise.h            #   Dirichlet noise
│   │   └── temperature_schedule.h  #   Temperature decay schedule
│   ├── infer/                      # Inference
│   │   └── onnx_policy_value_evaluator.*  # ONNX Runtime inference
│   └── runtime/                    # Runtime
│       ├── selfplay_runner.*       #   Self-play loop + FilteredRulesWrapper
│       ├── arena_runner.*          #   Model-vs-model arena
│       └── heuristic_runner.*      #   Heuristic game generation
│
├── games/                          # Game implementations (one dir per game)
│   ├── tictactoe/  quoridor/  splendor/  azul/  loveletter/  coup/
│
├── bindings/py_engine.cpp          # pybind11 Python bindings
│
├── training/                       # Python training framework
│   ├── pipeline.py   model.py   cli.py
│
├── platform/                       # FastAPI web platform + AI inference API
│   ├── app.py                      #   Main server
│   ├── ai_service/                 #   Observation-only AI REST API
│   ├── game_service/               #   Game sessions, async pipeline, replay analysis
│   └── static/                     #   Shared frontend assets
│
├── tests/                          # Parametrized tests (cover all registered games)
├── docs/                           # Documentation
└── setup.py · requirements.txt     # Build
```

---

## Model status

Shipped ONNX models per variant:

| Game | 2p | 3p | 4p |
|------|----|----|----|
| TicTacToe | trained | — | — |
| Quoridor | trained | — | — |
| Splendor | trained | **untrained (random init)** | **untrained (random init)** |
| Azul | trained | **untrained (random init)** | **untrained (random init)** |
| Love Letter | trained | trained | **untrained (random init)** |
| Coup | trained | **untrained (random init)** | **untrained (random init)** |

*Untrained* variants ship a randomly-initialized network — playable through the web UI but not competitive.
