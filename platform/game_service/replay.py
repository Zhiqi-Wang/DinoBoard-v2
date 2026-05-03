"""Replay frame construction and serialization.

Unified replay JSON format:
{
  "game_id": str,
  "seed": int,
  "players": {
    "player_0": {"name": str, "type": "human"|"model"|"heuristic"|"ai"},
    "player_1": {"name": str, "type": ...},
    ...  # N players
  },
  "result": {"winner": int, "draw": bool, "total_plies": int},
  "config": { ... },             # optional match config
  "action_history": [int, ...],
  "frames": [ ... ]              # optional; rebuilt from action_history if absent
}

Actor convention: "player_0", "player_1", ..., "player_N-1", "start".
"""
from __future__ import annotations

from typing import Any, Optional


def make_replay_frame(
    sess: dict,
    ply_index: int,
    actor: str,
    action_id: Optional[int],
    action_info: Optional[dict],
    ai_stats: Optional[dict] = None,
    analysis: Optional[dict] = None,
) -> dict:
    gs = sess["game_session"]
    return {
        "ply_index": ply_index,
        "actor": actor,
        "action_id": action_id,
        "action_info": action_info,
        "state": gs.get_state_dict(),
        "current_player": gs.current_player,
        "is_terminal": gs.is_terminal,
        "winner": gs.winner,
        "ai_stats": ai_stats,
        "analysis": analysis,
    }


def build_analysis_dict(
    best_wr: float,
    actual_wr: float,
    best_action: int,
    best_action_info: Optional[dict],
) -> dict:
    drop = max(0, (best_wr - actual_wr) * 100)
    return {
        "best_win_rate": round(best_wr, 4),
        "actual_win_rate": round(actual_wr, 4),
        "drop_score": round(drop, 1),
        "best_action": best_action,
        "best_action_info": best_action_info,
    }


def build_frames_from_actions(
    game_id: str,
    seed: int,
    action_history: list[int],
) -> list[dict]:
    """Replay action_history through a GameSession, producing frames."""
    import dinoboard_engine

    gs = dinoboard_engine.GameSession(game_id, seed)
    frames = [{
        "ply_index": 0,
        "actor": "start",
        "action_id": None,
        "action_info": None,
        "state": gs.get_state_dict(),
        "current_player": gs.current_player,
        "is_terminal": gs.is_terminal,
        "winner": gs.winner,
        "ai_stats": None,
        "analysis": None,
    }]

    for i, action_id in enumerate(action_history):
        player = gs.current_player
        action_info = gs.get_action_info(action_id)
        gs.apply_action(action_id)
        frames.append({
            "ply_index": i + 1,
            "actor": f"player_{player}",
            "action_id": action_id,
            "action_info": action_info,
            "state": gs.get_state_dict(),
            "current_player": gs.current_player,
            "is_terminal": gs.is_terminal,
            "winner": gs.winner,
            "ai_stats": None,
            "analysis": None,
        })

    return frames


def build_replay_dict(
    game_id: str,
    seed: int,
    action_history: list[int],
    players: dict[str, dict[str, Any]],
    result: dict[str, Any],
    config: Optional[dict[str, Any]] = None,
) -> dict:
    """Build a standard replay dict (without frames — lightweight for storage)."""
    replay: dict[str, Any] = {
        "game_id": game_id,
        "seed": seed,
        "players": players,
        "result": result,
        "action_history": action_history,
    }
    if config:
        replay["config"] = config
    return replay
