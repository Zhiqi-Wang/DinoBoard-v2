"""Tests for the replay module: frame building, replay dict construction, action replay."""
import sys
from pathlib import Path

import dinoboard_engine
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "platform"))
from game_service.replay import (
    build_analysis_dict,
    build_frames_from_actions,
    build_replay_dict,
    make_replay_frame,
)
from conftest import CANONICAL_GAMES, get_test_model


# ---------------------------------------------------------------------------
# build_frames_from_actions
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_build_frames_first_frame_is_start(game_id):
    """First frame should have actor='start' and no action."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=20,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions in episode")
    frames = build_frames_from_actions(game_id, 42, actions)
    assert frames[0]["actor"] == "start"
    assert frames[0]["action_id"] is None
    assert frames[0]["ply_index"] == 0


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_build_frames_count_matches_actions(game_id):
    """Number of frames should be len(action_history) + 1 (initial frame)."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=20,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions in episode")
    frames = build_frames_from_actions(game_id, 42, actions)
    assert len(frames) == len(actions) + 1


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_build_frames_actors_are_player_ids(game_id):
    """All non-start actors should be player_0 or player_1."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=20,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions in episode")
    frames = build_frames_from_actions(game_id, 42, actions)
    for frame in frames[1:]:
        assert frame["actor"].startswith("player_"), (
            f"non-start frame has unexpected actor: {frame['actor']}"
        )


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_build_frames_terminal_state_matches(game_id):
    """Last frame's terminal state should match the episode result."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=50,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions in episode")
    frames = build_frames_from_actions(game_id, 42, actions)
    last = frames[-1]
    if last["is_terminal"]:
        assert last["winner"] == ep["winner"]


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_build_frames_ply_indices_sequential(game_id):
    """Ply indices should be 0, 1, 2, ..."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=20,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions in episode")
    frames = build_frames_from_actions(game_id, 42, actions)
    for i, frame in enumerate(frames):
        assert frame["ply_index"] == i


def test_build_frames_empty_actions():
    """Empty action_history should produce a single start frame."""
    frames = build_frames_from_actions("tictactoe", 42, [])
    assert len(frames) == 1
    assert frames[0]["actor"] == "start"
    assert not frames[0]["is_terminal"]


# ---------------------------------------------------------------------------
# build_replay_dict
# ---------------------------------------------------------------------------

def test_build_replay_dict_structure():
    """Replay dict should have all required fields."""
    replay = build_replay_dict(
        game_id="tictactoe",
        seed=42,
        action_history=[0, 4, 1],
        players={
            "player_0": {"name": "Alice", "type": "model"},
            "player_1": {"name": "Bob", "type": "heuristic"},
        },
        result={"winner": 0, "draw": False, "total_plies": 3},
        config={"simulations": 100},
    )
    assert replay["game_id"] == "tictactoe"
    assert replay["seed"] == 42
    assert replay["action_history"] == [0, 4, 1]
    assert replay["players"]["player_0"]["name"] == "Alice"
    assert replay["players"]["player_1"]["type"] == "heuristic"
    assert replay["result"]["winner"] == 0
    assert replay["config"]["simulations"] == 100
    assert "frames" not in replay


def test_build_replay_dict_no_config():
    """config is optional."""
    replay = build_replay_dict(
        game_id="tictactoe",
        seed=1,
        action_history=[],
        players={
            "player_0": {"name": "A", "type": "model"},
            "player_1": {"name": "B", "type": "model"},
        },
        result={"winner": -1, "draw": True, "total_plies": 0},
    )
    assert "config" not in replay


# ---------------------------------------------------------------------------
# build_analysis_dict
# ---------------------------------------------------------------------------

def test_analysis_dict_no_drop_when_optimal():
    """If actual == best, drop should be 0."""
    analysis = build_analysis_dict(0.75, 0.75, 42, {"desc": "e2e4"})
    assert analysis["drop_score"] == 0.0
    assert analysis["best_action"] == 42


def test_analysis_dict_positive_drop():
    """Suboptimal move should have positive drop."""
    analysis = build_analysis_dict(0.80, 0.60, 5, None)
    assert analysis["drop_score"] == 20.0


def test_analysis_dict_fields():
    analysis = build_analysis_dict(0.9, 0.7, 1, {"desc": "Nf3"})
    assert "best_win_rate" in analysis
    assert "actual_win_rate" in analysis
    assert "drop_score" in analysis
    assert "best_action" in analysis
    assert "best_action_info" in analysis


# ---------------------------------------------------------------------------
# make_replay_frame
# ---------------------------------------------------------------------------

def test_make_replay_frame_structure():
    """make_replay_frame should produce a dict with all expected keys."""
    gs = dinoboard_engine.GameSession("tictactoe", seed=42)
    sess = {"game_session": gs}
    frame = make_replay_frame(sess, 0, "start", None, None)
    expected_keys = {
        "ply_index", "actor", "action_id", "action_info",
        "state", "current_player", "is_terminal", "winner",
        "ai_stats", "analysis",
    }
    assert set(frame.keys()) == expected_keys
    assert frame["ply_index"] == 0
    assert frame["actor"] == "start"
    assert not frame["is_terminal"]


# ---------------------------------------------------------------------------
# Round-trip: selfplay -> action_history -> rebuild frames
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_rebuild_frames_state_matches_direct_play(game_id):
    """Rebuilt frames should produce the same final state as playing the game directly."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id),
        simulations=10, max_game_plies=30,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    if not actions:
        pytest.skip("no actions")

    frames = build_frames_from_actions(game_id, 42, actions)

    gs = dinoboard_engine.GameSession(game_id, seed=42)
    for a in actions:
        gs.apply_action(a)

    last_frame = frames[-1]
    assert last_frame["state"] == gs.get_state_dict()
    assert last_frame["is_terminal"] == gs.is_terminal
    assert last_frame["winner"] == gs.winner
