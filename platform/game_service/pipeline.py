"""Async pipeline: precompute + analyze user move + AI thinking."""
from __future__ import annotations

import logging
import time
from concurrent.futures import ThreadPoolExecutor

import dinoboard_engine as engine

logger = logging.getLogger(__name__)

from .sessions import (
    history_hash,
    pipeline_mark_done_empty,
    pipeline_reset,
    precompute_clear,
)
from .replay import build_analysis_dict, make_replay_frame

PIPELINE_EXECUTOR = ThreadPoolExecutor(max_workers=2, thread_name_prefix="pipeline")
PRECOMPUTE_EXECUTOR = ThreadPoolExecutor(max_workers=2, thread_name_prefix="precompute")

DEFAULT_ANALYSIS_SIMULATIONS = 5000


def _human_wr_from_stats(stats: dict, human_player: int) -> float:
    """Extract human player's win rate for the best action (absolute player ordering)."""
    root_values = stats["root_values"]
    if human_player >= len(root_values):
        raise IndexError(
            f"_human_wr_from_stats: human_player={human_player} out of range "
            f"for root_values of length {len(root_values)}"
        )
    return (root_values[human_player] + 1) / 2


def _human_wr_for_action(stats: dict, action_id: int, human_player: int) -> float | None:
    """Extract human player's win rate for a specific action from action_values map.

    Returns None only when the action isn't in the search tree (e.g. filtered out);
    that's a legitimate "not available" signal rather than a missing-field bug.
    """
    action_values = stats["action_values"]
    if action_id not in action_values:
        return None
    vals = action_values[action_id]
    if human_player >= len(vals):
        raise IndexError(
            f"_human_wr_for_action: human_player={human_player} out of range "
            f"for action_values[{action_id}] of length {len(vals)}"
        )
    return (vals[human_player] + 1) / 2


def shutdown_executors() -> None:
    PIPELINE_EXECUTOR.shutdown(wait=False, cancel_futures=True)
    PRECOMPUTE_EXECUTOR.shutdown(wait=False, cancel_futures=True)


# ─── Precompute ───


def _precompute_worker(sess: dict, ply_index: int, expected_hash: str) -> None:
    try:
        actual_id = sess["actual_id"]
        seed = sess["seed"]
        model_path = sess["model_path"] if sess["use_model"] else ""
        action_history = list(sess["action_history"])
        if sess["precompute"]["history_hash"] != expected_hash:
            return

        gs = engine.GameSession(actual_id, seed, model_path, sess["ai_use_filter"])
        for aid in action_history:
            gs.apply_action(aid)
        if gs.is_terminal:
            return

        analysis_sims = sess.get("analysis_simulations", DEFAULT_ANALYSIS_SIMULATIONS)
        result = gs.get_ai_action(analysis_sims, 0.0)

        pc = sess["precompute"]
        if pc["history_hash"] == expected_hash and pc["ply_index"] == ply_index:
            pc["result"] = result
    except Exception:
        logger.exception("precompute_worker failed for %s", sess.get("actual_id", "?"))


def schedule_precompute(sess: dict) -> None:
    gs = sess["game_session"]
    if gs.is_terminal:
        return
    if gs.current_player in sess["ai_players"]:
        return
    if not sess["model_path"]:
        return

    ply_index = len(sess["action_history"])
    h = history_hash(sess["action_history"])
    pc = sess["precompute"]

    if pc["ply_index"] == ply_index and pc["history_hash"] == h:
        return

    pc["ply_index"] = ply_index
    pc["history_hash"] = h
    pc["result"] = None
    PRECOMPUTE_EXECUTOR.submit(_precompute_worker, sess, ply_index, h)


# ─── Pipeline ───


def _cancelled(sess: dict) -> bool:
    return sess["pipeline"]["phase"] == "done"


def _build_isolated_gs(sess: dict) -> engine.GameSession:
    """Isolated GameSession for AI search (honors the per-game action filter)."""
    model_path = sess["model_path"] if sess["use_model"] else ""
    gs = engine.GameSession(sess["actual_id"], sess["seed"], model_path,
                            sess["ai_use_filter"])
    for aid in sess["action_history"]:
        gs.apply_action(aid)
    return gs


def _get_ai_move(gs: engine.GameSession, sess: dict) -> dict:
    """Pick AI action using heuristic or MCTS depending on difficulty."""
    if sess["difficulty"] == "heuristic":
        result = gs.get_heuristic_action()
        if result and "action" in result:
            return result
    simulations = sess["simulations"]
    temperature = sess["temperature"]
    return gs.get_ai_action(simulations, temperature)


def _analyze_user_move(sess: dict) -> dict | None:
    """Drop-score analysis for the human's last move.

    Does NOT run its own MCTS. Consumes the precompute result that was
    already scheduled when the current human-to-play position was first
    reached (after the previous AI move, game start, or undo). The same
    precompute result also backs the "智能提示" (ai-hint) endpoint — one
    MCTS per human-to-play state, reused for both purposes.

    Flow: wait up to 8s for precompute to finish, then read the best
    action's Q value (best_wr) and the human's chosen action's Q value
    (actual_wr) from the cached `action_values` dict. Drop score is
    (best_wr - actual_wr) * 100.

    Fallback: if precompute never completed (unusual — cancelled or timed
    out), run an inline MCTS as a backstop so the analysis still resolves.
    This should be rare; the normal path is cache-hit.
    """
    action_history = list(sess["action_history"])
    if not action_history:
        return None

    human_action_id = action_history[-1]
    pre_move_history = action_history[:-1]
    pre_move_hash = history_hash(pre_move_history)
    human_player = sess["human_player"]

    # Wait for precompute (which already ran MCTS on the pre-move position)
    pc = sess["precompute"]
    deadline = time.time() + 8.0
    search_result = None
    while time.time() < deadline:
        if _cancelled(sess):
            return None
        if pc["history_hash"] == pre_move_hash and pc["result"] is not None:
            search_result = pc["result"]
            break
        time.sleep(0.05)

    if _cancelled(sess):
        return None

    # Fallback: inline MCTS if precompute missed
    if search_result is None:
        model_path = sess["model_path"] if sess["use_model"] else ""
        gs_pre = engine.GameSession(sess["actual_id"], sess["seed"], model_path,
                                    sess["ai_use_filter"])
        for aid in pre_move_history:
            gs_pre.apply_action(aid)
        if gs_pre.is_terminal:
            return None
        analysis_sims = sess.get("analysis_simulations", DEFAULT_ANALYSIS_SIMULATIONS)
        search_result = gs_pre.get_ai_action(analysis_sims, 0.0)

    if _cancelled(sess):
        return None

    stats = search_result["stats"]
    best_action = search_result["action"]
    best_action_info = search_result["action_info"]
    best_wr = _human_wr_from_stats(stats, human_player)

    if human_action_id == best_action:
        actual_wr = best_wr
    else:
        actual_wr = _human_wr_for_action(stats, human_action_id, human_player)
        if actual_wr is None:
            actual_wr = best_wr

    return build_analysis_dict(best_wr, actual_wr, best_action, best_action_info)


def _commit_ai_move(sess: dict, pipe: dict, ai_move: dict, acting_player: int,
                    analysis: dict | None) -> None:
    """Apply the AI move to session state. Holds pipeline_lock briefly.

    Split out so the heavy compute (MCTS in _get_ai_move, analysis in
    _analyze_user_move) can run without holding the lock. The lock is only
    held during this short commit, so cancel_pipeline returns quickly even
    when MCTS is mid-flight.
    """
    ai_action_id = ai_move["action"]
    ai_action_info = ai_move["action_info"]
    ai_stats = ai_move.get("stats")

    sess["game_session"].apply_action(ai_action_id)
    sess["action_history"].append(ai_action_id)
    ply = len(sess["action_history"])
    sess["replay_frames"].append(
        make_replay_frame(sess, ply, f"player_{acting_player}", ai_action_id, ai_action_info,
                          ai_stats=ai_stats, analysis=None))

    if analysis and sess["replay_frames"]:
        human_actor = f"player_{sess['human_player']}"
        for frame in reversed(sess["replay_frames"][:-1]):
            if frame["actor"] == human_actor:
                frame["analysis"] = analysis
                break

    pipe["analysis"] = analysis
    pipe["ai_action"] = ai_action_id
    pipe["ai_action_info"] = ai_action_info
    pipe["ai_stats"] = ai_stats


def _pipeline_worker(sess: dict, session_id: str) -> None:
    """Compute analysis + AI move outside the lock; commit atomically under lock.

    Lock scope: only the final commit. Cancellation is cooperative and checked
    before commit; if cancelled, all computed work is discarded and session
    state is never mutated.
    """
    try:
        pipe = sess["pipeline"]

        # Phase 1: analyze user move (outside lock — just reads history).
        pipe["phase"] = "analyzing"
        analysis = None
        gs_check = _build_isolated_gs(sess)
        if not gs_check.is_terminal:
            analysis = _analyze_user_move(sess)
            if _cancelled(sess):
                return

        # Phase 2: AI thinking — runs on isolated GameSession, outside lock.
        pipe["phase"] = "ai_thinking"
        ai_move = None
        acting_player = -1
        gs_check = _build_isolated_gs(sess)
        if not gs_check.is_terminal and gs_check.current_player in sess["ai_players"]:
            acting_player = gs_check.current_player
            ai_move = _get_ai_move(gs_check, sess)
            if _cancelled(sess):
                return

        # Commit phase: hold lock briefly, re-check cancellation, apply.
        lock = sess["pipeline_lock"]
        if not lock.acquire(timeout=12):
            logger.warning("pipeline_worker: commit-lock timeout for session %s", session_id)
            return
        try:
            if _cancelled(sess):
                return
            if ai_move is not None:
                _commit_ai_move(sess, pipe, ai_move, acting_player, analysis)
            else:
                pipe["analysis"] = analysis
                pipe["ai_action"] = None
                pipe["ai_action_info"] = None
                pipe["ai_stats"] = None
            pipe["phase"] = "done"
            precompute_clear(sess)
        finally:
            lock.release()

        schedule_precompute(sess)

    except Exception as exc:
        logger.exception("pipeline_worker failed for session %s", session_id)
        sess["pipeline"]["phase"] = "error"
        sess["pipeline"]["error"] = str(exc)
        sess["pipeline"]["ai_action"] = None


def _pipeline_worker_ai_only(sess: dict, session_id: str) -> None:
    """AI-only variant: single AI move, no analysis."""
    try:
        pipe = sess["pipeline"]
        pipe["phase"] = "ai_thinking"

        ai_move = None
        acting_player = -1
        gs_check = _build_isolated_gs(sess)
        if not gs_check.is_terminal and gs_check.current_player in sess["ai_players"]:
            acting_player = gs_check.current_player
            ai_move = _get_ai_move(gs_check, sess)
            if _cancelled(sess):
                return

        lock = sess["pipeline_lock"]
        if not lock.acquire(timeout=12):
            logger.warning("pipeline_worker_ai_only: commit-lock timeout for session %s", session_id)
            return
        try:
            if _cancelled(sess):
                return
            if ai_move is not None:
                _commit_ai_move(sess, pipe, ai_move, acting_player, None)
            else:
                pipe["ai_action"] = None
                pipe["ai_action_info"] = None
                pipe["ai_stats"] = None
            pipe["phase"] = "done"
            precompute_clear(sess)
        finally:
            lock.release()

        schedule_precompute(sess)

    except Exception as exc:
        logger.exception("pipeline_worker_ai_only failed for session %s", session_id)
        sess["pipeline"]["phase"] = "error"
        sess["pipeline"]["error"] = str(exc)
        sess["pipeline"]["ai_action"] = None


def schedule_pipeline(sess: dict, session_id: str) -> None:
    pipe = sess["pipeline"]
    if pipe["phase"] in ("analyzing", "ai_thinking"):
        return
    pipe["phase"] = "queued"
    pipe["ai_action"] = None
    pipe["ai_action_info"] = None
    pipe["ai_stats"] = None
    PIPELINE_EXECUTOR.submit(_pipeline_worker, sess, session_id)


def schedule_pipeline_ai_only(sess: dict, session_id: str) -> None:
    pipe = sess["pipeline"]
    if pipe["phase"] in ("analyzing", "ai_thinking"):
        return
    pipe["phase"] = "queued"
    pipe["ai_action"] = None
    pipe["ai_action_info"] = None
    pipe["ai_stats"] = None
    PIPELINE_EXECUTOR.submit(_pipeline_worker_ai_only, sess, session_id)


def signal_cancel(sess: dict) -> None:
    """Non-blocking cancel of the PIPELINE only: signal workers to stop.

    Does NOT touch the precompute — precompute runs in a separate executor
    and its cached result is needed by _analyze_user_move right after the
    cancel. Clearing precompute here caused a bug where apply_action's
    opening cancel_pipeline() wiped the game-start precompute, forcing
    _analyze_user_move to time-wait 8s on an empty cache (observed as
    "AI takes ~10 seconds to respond to the first move"). Callers that
    genuinely need to invalidate precompute (undo / state rebuild) must
    call precompute_clear() explicitly.
    """
    pipeline_mark_done_empty(sess)


def cancel_pipeline(sess: dict) -> None:
    signal_cancel(sess)
    lock = sess["pipeline_lock"]
    lock.acquire()
    lock.release()
    pipeline_reset(sess)
