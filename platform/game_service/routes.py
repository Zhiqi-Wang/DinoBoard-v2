"""Game API routes."""
from __future__ import annotations

from pathlib import Path

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from .sessions import (
    GAME_CONFIGS,
    create_session,
    get_session,
    rebuild_game_session,
    session_response,
    precompute_clear,
)
from .replay import make_replay_frame, build_frames_from_actions
from .pipeline import (
    schedule_precompute,
    schedule_pipeline,
    schedule_pipeline_ai_only,
    cancel_pipeline,
    signal_cancel,
    _build_isolated_gs,
)

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

router = APIRouter(prefix="/api/games", tags=["games"])


class CreateGameRequest(BaseModel):
    game_id: str
    seed: int = 0
    human_player: int = 0
    num_players: int = 2
    difficulty: str = "expert"


class ActionRequest(BaseModel):
    action_id: int


_GAME_DISPLAY_ORDER = ["azul", "splendor", "quoridor", "loveletter", "coup", "tictactoe"]


@router.get("/available")
def available_games():
    order_index = {gid: i for i, gid in enumerate(_GAME_DISPLAY_ORDER)}
    sorted_items = sorted(
        GAME_CONFIGS.items(),
        key=lambda kv: (order_index.get(kv[0], len(_GAME_DISPLAY_ORDER)), kv[0]),
    )
    games = []
    for gid, cfg in sorted_items:
        has_web = (PROJECT_ROOT / "games" / gid / "web" / "index.html").exists()
        games.append({
            "game_id": gid,
            "display_name": cfg.get("display_name", gid),
            "players": cfg["players"],
            "has_web": has_web,
        })
    return {"games": games}


@router.post("")
def create_game(req: CreateGameRequest):
    try:
        session_id, sess = create_session(
            req.game_id, req.seed, req.human_player, req.num_players, req.difficulty)
    except Exception as e:
        raise HTTPException(400, str(e))

    sess["replay_frames"].append(make_replay_frame(sess, 0, "start", None, None))

    gs = sess["game_session"]
    if not gs.is_terminal and gs.current_player in sess["ai_players"]:
        pass
    elif req.difficulty == "expert":
        schedule_precompute(sess)

    return session_response(session_id, sess)


@router.get("/{session_id}")
def get_game_state(session_id: str):
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    return session_response(session_id, sess)


@router.post("/{session_id}/action")
def apply_action(session_id: str, req: ActionRequest):
    """Human move. Returns immediately; pipeline runs in background."""
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    gs = sess["game_session"]
    if gs.is_terminal:
        raise HTTPException(400, "game is already over")

    # Must wait for any running pipeline worker to finish before mutating
    # sess["game_session"] / action_history, otherwise the worker's late
    # writes race with the human move (duplicate AI moves, KeyError on
    # ai_move["action"], etc.). cancel_pipeline acquires the session's
    # pipeline_lock, which the worker holds for its entire run.
    cancel_pipeline(sess)

    action_info = gs.get_action_info(req.action_id)
    acting_player = gs.current_player

    try:
        gs.apply_action(req.action_id)
    except Exception as e:
        raise HTTPException(400, str(e))
    sess["action_history"].append(req.action_id)
    ply = len(sess["action_history"])
    sess["replay_frames"].append(
        make_replay_frame(sess, ply, f"player_{acting_player}", req.action_id, action_info, analysis=None))

    if not gs.is_terminal and gs.current_player in sess["ai_players"]:
        if sess["difficulty"] == "expert":
            schedule_pipeline(sess, session_id)
        else:
            schedule_pipeline_ai_only(sess, session_id)
    elif not gs.is_terminal and sess["difficulty"] == "expert":
        schedule_precompute(sess)

    return session_response(session_id, sess, {"action_info": action_info})


@router.post("/{session_id}/ai-action")
def trigger_ai_action(session_id: str):
    """Trigger AI-only pipeline (for AI-first games). Returns immediately."""
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    gs = sess["game_session"]
    if gs.is_terminal:
        raise HTTPException(400, "game is already over")
    if gs.current_player not in sess["ai_players"]:
        raise HTTPException(400, "not AI's turn")

    schedule_pipeline_ai_only(sess, session_id)
    return session_response(session_id, sess)


@router.get("/{session_id}/pipeline")
def get_pipeline_status(session_id: str):
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    pipe = sess["pipeline"]
    resp = {
        "phase": pipe["phase"],
        "ai_action": pipe["ai_action"],
        "ai_action_info": pipe["ai_action_info"],
        "ai_stats": pipe["ai_stats"],
        "analysis": pipe.get("analysis"),
    }
    if "error" in pipe:
        resp["error"] = pipe["error"]
    return resp


@router.post("/{session_id}/step-back")
def step_back(session_id: str):
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    history = sess["action_history"]
    if not history:
        raise HTTPException(400, "no moves to undo")

    cancel_pipeline(sess)

    if len(sess["replay_frames"]) > 1:
        sess["replay_frames"].pop()

    history.pop()

    rebuild_game_session(sess)
    precompute_clear(sess)
    if sess["difficulty"] == "expert":
        schedule_precompute(sess)
    return session_response(session_id, sess)


@router.post("/{session_id}/ai-hint")
def ai_hint(session_id: str):
    import time

    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    gs = sess["game_session"]
    if gs.is_terminal:
        raise HTTPException(400, "game is already over")

    try:
        if sess["difficulty"] == "expert":
            pc = sess["precompute"]
            deadline = time.time() + 8.0
            while time.time() < deadline:
                if pc["result"] is not None:
                    break
                time.sleep(0.05)
            if pc["result"] is not None:
                hint = pc["result"]
            else:
                # Use isolated AI-session so the hint respects ai_use_action_filter.
                ai_gs = _build_isolated_gs(sess)
                hint = ai_gs.get_ai_action(sess["simulations"], 0.0)
        elif sess["difficulty"] == "heuristic":
            hint = gs.get_heuristic_action()
        else:
            ai_gs = _build_isolated_gs(sess)
            simulations = sess["simulations"]
            hint = ai_gs.get_ai_action(simulations, 0.0)
    except Exception as e:
        raise HTTPException(400, str(e))

    result = {
        "action": hint["action"],
        "action_info": hint["action_info"],
    }
    if "stats" in hint:
        result["stats"] = hint["stats"]
    return result


@router.get("/{session_id}/replay")
def get_replay(session_id: str):
    try:
        sess = get_session(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    gs = sess["game_session"]
    human_p = sess["human_player"]
    players = {}
    for p in range(gs.num_players):
        if p == human_p:
            players[f"player_{p}"] = {"name": "玩家", "type": "human"}
        else:
            players[f"player_{p}"] = {"name": f"AI {p}", "type": "ai"}
    return {
        "game_id": sess["actual_id"],
        "seed": sess["seed"],
        "players": players,
        "frames": sess["replay_frames"],
    }


replay_router = APIRouter(prefix="/api/replay", tags=["replay"])


class BuildReplayRequest(BaseModel):
    game_id: str
    seed: int
    action_history: list[int]


@replay_router.post("/build")
def build_replay(req: BuildReplayRequest):
    """Build frames from action_history (for replays that only store actions)."""
    try:
        frames = build_frames_from_actions(req.game_id, req.seed, req.action_history)
    except Exception as e:
        raise HTTPException(400, str(e))
    return {"frames": frames}


@replay_router.get("/file")
def get_replay_file(path: str):
    import json
    resolved = (PROJECT_ROOT / path).resolve()
    if not str(resolved).startswith(str(PROJECT_ROOT)):
        raise HTTPException(403, "path outside project root")
    if not resolved.exists():
        raise HTTPException(404, f"file not found: {path}")
    try:
        data = json.loads(resolved.read_text())
    except Exception as e:
        raise HTTPException(400, f"invalid JSON: {e}")
    if "frames" not in data and "action_history" not in data:
        raise HTTPException(400, "replay file must contain 'frames' or 'action_history'")
    return data
