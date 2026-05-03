"""Tests for the tail solver."""
import dinoboard_engine
import pytest

from conftest import get_test_model


def test_tail_solve_returns_valid_fields_quoridor():
    r = dinoboard_engine.tail_solve(
        game_id="quoridor", seed=42, perspective_player=0,
        depth_limit=5, node_budget=10000,
    )
    assert "value" in r
    assert "best_action" in r
    assert "nodes_searched" in r
    assert "budget_exceeded" in r
    assert r["nodes_searched"] >= 0
    assert -2.0 <= r["value"] <= 2.0


def test_tail_solve_tiny_budget_exceeds_quoridor():
    r = dinoboard_engine.tail_solve(
        game_id="quoridor", seed=42, perspective_player=0,
        depth_limit=20, node_budget=5,
    )
    assert r["budget_exceeded"], "tiny budget should exceed"


def test_tail_solve_no_solver_raises_tictactoe():
    with pytest.raises(RuntimeError, match="no tail_solver registered"):
        dinoboard_engine.tail_solve(
            game_id="tictactoe", seed=42, perspective_player=0,
            depth_limit=12, node_budget=1000000,
        )


def test_selfplay_tail_solve_stats_invariant_quoridor():
    """Tail solve stats invariant: successes <= completed <= attempts.

    Quoridor trigger requires ply >= 20 AND shortest_path <= 4. With uniform
    policy (10 sims) the game might not reach trigger conditions, so we only
    check the invariant, not that attempts > 0.
    """
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=200, tail_solve_enabled=True, tail_solve_start_ply=1,
        tail_solve_depth_limit=3, tail_solve_node_budget=500,
    )
    a = ep["tail_solve_attempts"]
    c = ep["tail_solve_completed"]
    s = ep["tail_solve_successes"]
    assert s <= c <= a, f"invariant violated: {s} <= {c} <= {a}"
