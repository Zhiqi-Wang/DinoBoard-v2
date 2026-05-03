"""Tests for heuristic episodes and evaluation functions."""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, get_test_model, run_short_heuristic


def test_heuristic_episode_valid_samples_quoridor():
    ep = run_short_heuristic("quoridor")
    cfg = GAME_CONFIGS["quoridor"]
    assert len(ep["samples"]) > 0, "heuristic episode produced no samples"
    for s in ep["samples"]:
        assert len(s["features"]) == cfg["feature_dim"]
        assert len(s["legal_mask"]) == cfg["action_space"]
    if len(ep["samples"]) >= 3:
        assert ep["samples"][0]["features"] != ep["samples"][2]["features"], (
            "heuristic features should vary across plies"
        )


def test_constrained_eval_returns_valid_result_quoridor():
    model = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=model, simulations=10,
        model_is_player=0, constrained=True, heuristic_temperature=0.0,
    )
    assert "winner" in r
    assert "draw" in r
    assert "total_plies" in r
    assert r["winner"] in (-1, 0, 1)


def test_free_eval_returns_valid_result_quoridor():
    model = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=model, simulations=10,
        model_is_player=0, constrained=False, heuristic_temperature=0.0,
    )
    assert "winner" in r
    assert "draw" in r
    assert r["winner"] in (-1, 0, 1)


def test_arena_match_returns_valid_result(game_id):
    model = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[model, model],
        simulations_list=[5, 5], temperature=0.0,
    )
    assert "winner" in r
    assert "draw" in r
    assert "total_plies" in r
    assert r["total_plies"] > 0


def test_eval_vs_heuristic_win_count_adds_up():
    from training.pipeline import run_eval_vs_heuristic

    num_games = 4
    model = get_test_model("quoridor")
    result = run_eval_vs_heuristic(
        game_id="quoridor", model_path=model, num_games=num_games,
        base_seed=42, simulations=5, constrained=True,
        heuristic_temperature=0.0, max_workers=1,
    )
    total = result["wins"] + result["losses"] + result["draws"]
    assert total == num_games, f"expected {num_games}, got {total}"
    assert 0.0 <= result["win_rate"] <= 1.0
