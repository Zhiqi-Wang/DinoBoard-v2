"""In-memory game session management."""
from __future__ import annotations

import json
import sys
import threading
import uuid
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

import dinoboard_engine as engine

sessions: dict = {}

DIFFICULTY_PRESETS = {
    "heuristic": {"simulations": 1, "temperature": 0.0, "use_model": False},
    "casual": {"simulations": 10, "temperature": 0.0, "use_model": True},
    "expert": {"simulations": 5000, "temperature": 0.0, "use_model": True},
}


def load_game_configs() -> dict:
    configs = {}
    games_dir = PROJECT_ROOT / "games"
    for game_dir in sorted(games_dir.iterdir()):
        config_path = game_dir / "config" / "game.json"
        if config_path.exists():
            with open(config_path) as f:
                cfg = json.load(f)
            configs[cfg["game_id"]] = cfg
    return configs


def load_web_configs() -> dict:
    """Load per-game web.json configs. Falls back to game.json legacy fields."""
    configs = {}
    games_dir = PROJECT_ROOT / "games"
    for game_dir in sorted(games_dir.iterdir()):
        web_path = game_dir / "config" / "web.json"
        game_path = game_dir / "config" / "game.json"
        if web_path.exists():
            with open(web_path) as f:
                configs[game_dir.name] = json.load(f)
        elif game_path.exists():
            with open(game_path) as f:
                game_cfg = json.load(f)
            fallback = dict(game_cfg.get("web", {}))
            if game_cfg.get("ai_use_action_filter"):
                fallback["ai_use_action_filter"] = True
            if fallback:
                configs[game_dir.name] = fallback
    return configs


GAME_CONFIGS = load_game_configs()
WEB_CONFIGS = load_web_configs()


def _base_game_id(game_id: str) -> str:
    """Return the base game (stripping a trailing _<N>p variant suffix).

    Examples: 'azul' → 'azul', 'azul_3p' → 'azul', 'splendor_2p' → 'splendor'.
    """
    import re
    return re.sub(r"_\d+p$", "", game_id)


def find_model_path(game_id: str) -> str:
    """Return the path to the game's deployed model, or "" if none exists.

    Convention: games/<base>/model/<variant>.onnx, where <variant> is the
    game_id (e.g. azul_2p.onnx, azul_3p.onnx). All variants of a game live
    in the same model directory — no separate games/<name>_3p/ subdirs.

    The base 2p game_id also maps to its explicit '<game>_2p.onnx' file
    — callers pass either the bare base id ('azul') or the explicit
    variant id ('azul_2p') and get the same file.
    """
    base = _base_game_id(game_id)
    model_dir = PROJECT_ROOT / "games" / base / "model"

    # Primary: <variant>.onnx (e.g. azul_3p.onnx). If game_id is already
    # the base form, treat it as the 2p variant.
    variant_name = game_id if game_id != base else f"{base}_2p"
    candidate = model_dir / f"{variant_name}.onnx"
    if candidate.exists():
        return str(candidate)

    # Also try the bare variant form in case the caller passed 'azul_2p'
    # but we only got 'azul.onnx' on disk. Harmless if nothing matches.
    alt = model_dir / f"{game_id}.onnx"
    if alt.exists():
        return str(alt)

    return ""


def get_session(session_id: str) -> dict:
    sess = sessions.get(session_id)
    if not sess:
        raise KeyError(f"session {session_id} not found")
    return sess


def _make_pipeline_state() -> dict:
    return {
        "phase": "idle",
        "ai_action": None,
        "ai_action_info": None,
        "ai_stats": None,
        "analysis": None,
    }


def _make_precompute_state() -> dict:
    return {
        "ply_index": -1,
        "history_hash": "",
        "result": None,
    }


def create_session(
    game_id: str,
    seed: int,
    human_player: int,
    num_players: int,
    difficulty: str,
) -> tuple[str, dict]:
    if difficulty not in DIFFICULTY_PRESETS:
        raise ValueError(f"unknown difficulty {difficulty!r}, expected one of {list(DIFFICULTY_PRESETS)}")
    preset = DIFFICULTY_PRESETS[difficulty]

    actual_id = game_id
    if num_players != 2:
        candidate = f"{game_id}_{num_players}p"
        if candidate in engine.available_games():
            actual_id = candidate
        elif game_id not in GAME_CONFIGS:
            actual_id = candidate

    model_path = ""
    if preset["use_model"]:
        model_path = find_model_path(actual_id)
        if not model_path:
            base = _base_game_id(actual_id)
            raise FileNotFoundError(
                f"no trained model found for {actual_id}. "
                f"Expected at games/{base}/model/{actual_id}.onnx. "
                f"Copy from runs/<run_name>/models/model_best.onnx after training, "
                f"renaming to <variant>.onnx."
            )

    gs = engine.GameSession(actual_id, seed, model_path, False)

    if game_id not in GAME_CONFIGS:
        raise ValueError(f"unknown game_id {game_id!r}, not found in GAME_CONFIGS")

    web_cfg = WEB_CONFIGS.get(game_id, {})
    ai_use_filter = bool(web_cfg.get("ai_use_action_filter", False))

    actual_num_players = gs.num_players
    ai_players = [p for p in range(actual_num_players) if p != human_player]

    diff_overrides = web_cfg.get("difficulty_overrides", {}).get(difficulty, {})
    effective_sims = diff_overrides.get("simulations", preset["simulations"])
    effective_temp = diff_overrides.get("temperature", preset["temperature"])
    analysis_sims = web_cfg.get("analysis_simulations", 5000)

    tail_cfg = web_cfg.get("tail_solve", {})
    if tail_cfg.get("enabled", False):
        gs.configure_tail_solve(
            True,
            tail_cfg.get("depth_limit", 10),
            tail_cfg.get("node_budget", 200000),
        )

    session_id = uuid.uuid4().hex[:12]
    sess = {
        "game_session": gs,
        "human_player": human_player,
        "ai_players": ai_players,
        "ai_player": ai_players[0] if ai_players else -1,
        "game_id": game_id,
        "actual_id": actual_id,
        "seed": seed,
        "difficulty": difficulty,
        "simulations": effective_sims,
        "temperature": effective_temp,
        "analysis_simulations": analysis_sims,
        "use_model": preset["use_model"],
        "model_path": model_path,
        "ai_use_filter": ai_use_filter,
        "action_history": [],
        "replay_frames": [],
        "pipeline": _make_pipeline_state(),
        "precompute": _make_precompute_state(),
        "pipeline_lock": threading.Lock(),
    }
    sessions[session_id] = sess
    return session_id, sess


def rebuild_game_session(sess: dict) -> None:
    """Recreate GameSession and replay action_history. The live session is for
    humans, so use_filter is always False."""
    model_path = sess["model_path"] if sess["use_model"] else ""
    gs = engine.GameSession(sess["actual_id"], sess["seed"], model_path, False)
    for aid in sess["action_history"]:
        gs.apply_action(aid)
    sess["game_session"] = gs


def session_response(session_id: str, sess: dict, extra: Optional[dict] = None) -> dict:
    gs = sess["game_session"]
    current = gs.current_player
    legal = gs.get_legal_actions()
    last_actor = None
    frames = sess.get("replay_frames")
    if frames and len(frames) > 0:
        last_actor = frames[-1].get("actor")

    result = {
        "session_id": session_id,
        "state": gs.get_state_dict(),
        "legal_actions": legal,
        "current_player": current,
        "is_terminal": gs.is_terminal,
        "is_turn_start": gs.is_turn_start,
        "last_actor": last_actor,
        "winner": gs.winner,
        "num_players": gs.num_players,
        "human_player": sess["human_player"],
        "ai_players": sess["ai_players"],
        "ai_player": sess["ai_player"],
        "difficulty": sess["difficulty"],
    }
    if extra:
        result.update(extra)
    return result


def pipeline_mark_done_empty(sess: dict) -> None:
    p = sess["pipeline"]
    p["phase"] = "done"
    p["ai_action"] = None
    p["ai_action_info"] = None
    p["ai_stats"] = None
    p["analysis"] = None


def pipeline_reset(sess: dict) -> None:
    p = sess["pipeline"]
    p["phase"] = "idle"
    p["ai_action"] = None
    p["ai_action_info"] = None
    p["ai_stats"] = None
    p["analysis"] = None


def precompute_clear(sess: dict) -> None:
    pc = sess["precompute"]
    pc["ply_index"] = -1
    pc["history_hash"] = ""
    pc["result"] = None


def history_hash(action_history: list) -> str:
    return str(hash(tuple(action_history)))
