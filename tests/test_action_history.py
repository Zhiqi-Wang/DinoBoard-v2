"""Tests for action_history in arena/eval episodes and sample action_id consistency.

Arena and eval runners return action_history directly.
Selfplay and heuristic runners store action_ids in samples.
Both must be consistent and deterministically replayable.
"""
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, get_test_model


# ---------------------------------------------------------------------------
# Arena and eval: action_history field
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_has_action_history(game_id):
    """Arena matches should return action_history."""
    m = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert "action_history" in r
    assert isinstance(r["action_history"], (list, tuple))
    assert len(r["action_history"]) == r["total_plies"]


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_action_history_in_range(game_id):
    """All action IDs in arena history should be within valid range."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    action_space = info["action_space"]
    m = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    for a in r["action_history"]:
        assert 0 <= a < action_space, f"action {a} out of range [0, {action_space})"


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_action_history_replayable(game_id):
    """Replaying action_history through GameSession should produce matching terminal state."""
    m = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    actions = list(r["action_history"])
    if not actions:
        pytest.skip("empty game")

    gs = dinoboard_engine.GameSession(game_id, seed=42)
    for a in actions:
        gs.apply_action(a)

    if gs.is_terminal:
        assert gs.winner == r["winner"]


def test_constrained_eval_has_action_history():
    """Constrained eval should return action_history."""
    m = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, model_is_player=0,
        constrained=True, heuristic_temperature=0.0,
    )
    assert "action_history" in r
    assert len(r["action_history"]) == r["total_plies"]


# ---------------------------------------------------------------------------
# Selfplay: action_ids from samples
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_selfplay_sample_action_ids_replayable(game_id):
    """Replaying sample action_ids through GameSession should match episode result."""
    m = get_test_model(game_id)
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=m,
        simulations=10, max_game_plies=50,
    )
    actions = [s["action_id"] for s in ep["samples"]]
    assert len(actions) == ep["total_plies"]

    if not actions:
        pytest.skip("empty episode")

    gs = dinoboard_engine.GameSession(game_id, seed=42)
    for a in actions:
        assert not gs.is_terminal
        gs.apply_action(a)

    # If the game reached natural termination (not adjudicated), check consistency.
    # Adjudicated games have a winner but gs won't be terminal since adjudicator
    # only runs inside the selfplay runner, not in GameSession replay.
    if gs.is_terminal:
        assert gs.winner == ep["winner"]


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_selfplay_sample_action_ids_in_range(game_id):
    """All sample action IDs should be within valid range."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    action_space = info["action_space"]
    m = get_test_model(game_id)
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=m,
        simulations=10, max_game_plies=30,
    )
    for s in ep["samples"]:
        assert 0 <= s["action_id"] < action_space


# ---------------------------------------------------------------------------
# z_values consistency (including adjudicated games)
# ---------------------------------------------------------------------------

def test_adjudicated_game_has_z_values():
    """Adjudicated (non-terminal) games should still have z_values on samples."""
    m = get_test_model("quoridor")
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, max_game_plies=5,
    )
    if ep["total_plies"] < 5:
        pytest.skip("game ended before adjudication")
    for s in ep["samples"]:
        z_vals = s["z_values"]
        assert len(z_vals) == 2, f"ply {s['ply']}: z_values should have 2 elements, got {len(z_vals)}"


def test_terminal_game_has_z_values():
    """Terminal games should have z_values on all samples."""
    m = get_test_model("tictactoe")
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="tictactoe", seed=42, model_path=m,
        simulations=30, max_game_plies=9,
    )
    for s in ep["samples"]:
        z_vals = s["z_values"]
        assert len(z_vals) == 2, f"ply {s['ply']}: z_values should have 2 elements"
