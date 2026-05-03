"""Arena runner tests: side-swapping, asymmetric simulations, winner attribution."""
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, GAME_CONFIGS, GAMES_WITH_HEURISTIC, get_test_model


# ---------------------------------------------------------------------------
# Basic arena functionality
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_returns_valid_result(game_id):
    """Arena match should return winner, draw, total_plies."""
    m = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert "winner" in r
    assert "draw" in r
    assert "total_plies" in r
    assert r["total_plies"] > 0


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_winner_valid_range(game_id):
    """Winner should be -1 (draw) or a valid player index."""
    m = get_test_model(game_id)
    r = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    w = r["winner"]
    num_players = GAME_CONFIGS[game_id]["players"]["max"]
    assert w == -1 or (0 <= w < num_players), f"invalid winner {w}"


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_determinism(game_id):
    """Same seed should produce identical arena results."""
    m = get_test_model(game_id)
    r1 = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=12345,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    r2 = dinoboard_engine.run_arena_match(
        game_id=game_id, seed=12345,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert r1["winner"] == r2["winner"]
    assert r1["total_plies"] == r2["total_plies"]
    assert r1["draw"] == r2["draw"]


# ---------------------------------------------------------------------------
# Asymmetric simulations
# ---------------------------------------------------------------------------

def test_arena_asymmetric_simulations():
    """Player with more simulations should win more often in simple games."""
    m = get_test_model("tictactoe")
    strong_wins = 0
    for seed in range(30):
        r = dinoboard_engine.run_arena_match(
            game_id="tictactoe", seed=seed,
            model_paths=[m, m],
            simulations_list=[200, 5], temperature=0.0,
        )
        if r["winner"] == 0:
            strong_wins += 1
    assert strong_wins >= 5, f"stronger player only won {strong_wins}/30"


def test_arena_both_sides_get_turns():
    """Arena should have both players take turns."""
    m = get_test_model("tictactoe")
    r = dinoboard_engine.run_arena_match(
        game_id="tictactoe", seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert r["total_plies"] >= 3, "game too short — both players should play"


# ---------------------------------------------------------------------------
# Side-swapping correctness (pipeline eval logic)
# ---------------------------------------------------------------------------

def test_eval_batch_side_swap_logic():
    """Verify the pipeline's side-swapping evaluation logic."""
    from training.pipeline import run_eval_batch

    m = get_test_model("tictactoe")
    r = run_eval_batch(
        game_id="tictactoe",
        candidate_path=m,
        opponent_path=m,
        num_games=10,
        base_seed=42,
        sims_candidate=10,
        sims_opponent=10,
        max_workers=1,
    )
    total = r["wins"] + r["losses"] + r["draws"]
    assert total == 10, f"expected 10 games, got {total}"
    assert r["win_rate"] >= 0.0
    assert r["win_rate"] <= 1.0


def test_eval_vs_heuristic_side_swap_logic():
    """Eval vs heuristic alternates model_is_player 0/1."""
    from training.pipeline import run_eval_vs_heuristic

    m = get_test_model("quoridor")
    r = run_eval_vs_heuristic(
        game_id="quoridor",
        model_path=m,
        num_games=6,
        base_seed=42,
        simulations=10,
        constrained=True,
        heuristic_temperature=0.0,
        max_workers=1,
    )
    total = r["wins"] + r["losses"] + r["draws"]
    assert total == 6
    assert 0.0 <= r["win_rate"] <= 1.0


def test_eval_vs_heuristic_constrained_vs_free():
    """Constrained and free eval may produce different results."""
    from training.pipeline import run_eval_vs_heuristic

    m = get_test_model("quoridor")
    cr = run_eval_vs_heuristic(
        game_id="quoridor", model_path=m, num_games=10,
        base_seed=42, simulations=10, constrained=True,
        heuristic_temperature=0.0, max_workers=1,
    )
    fr = run_eval_vs_heuristic(
        game_id="quoridor", model_path=m, num_games=10,
        base_seed=42, simulations=10, constrained=False,
        heuristic_temperature=0.0, max_workers=1,
    )
    assert cr["wins"] + cr["losses"] + cr["draws"] == 10
    assert fr["wins"] + fr["losses"] + fr["draws"] == 10


# ---------------------------------------------------------------------------
# Arena with different seeds should produce variety
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_arena_different_seeds_vary(game_id):
    """Different seeds should produce different arena results."""
    m = get_test_model(game_id)
    results = []
    for seed in range(20):
        r = dinoboard_engine.run_arena_match(
            game_id=game_id, seed=seed,
            model_paths=[m, m],
            simulations_list=[10, 10], temperature=1.0,
        )
        results.append((r["winner"], r["total_plies"]))

    unique = len(set(results))
    assert unique >= 2, f"all 20 seeds produced identical results"


# ---------------------------------------------------------------------------
# Arena with hidden info games
# ---------------------------------------------------------------------------

def test_arena_splendor_hidden_info():
    """Arena for hidden-info game (Splendor) should complete."""
    m = get_test_model("splendor")
    r = dinoboard_engine.run_arena_match(
        game_id="splendor", seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert r["total_plies"] > 0


def test_arena_azul_hidden_info():
    """Arena for hidden-info game (Azul) should complete."""
    m = get_test_model("azul")
    r = dinoboard_engine.run_arena_match(
        game_id="azul", seed=42,
        model_paths=[m, m],
        simulations_list=[10, 10], temperature=0.0,
    )
    assert r["total_plies"] > 0


# ---------------------------------------------------------------------------
# Constrained eval edge cases
# ---------------------------------------------------------------------------

def test_constrained_eval_returns_all_fields():
    """run_constrained_eval_vs_heuristic should return winner, draw, total_plies."""
    m = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, model_is_player=0,
        constrained=True, heuristic_temperature=0.0,
    )
    assert "winner" in r
    assert "draw" in r
    assert "total_plies" in r


def test_constrained_eval_model_as_player1():
    """Model can play as player 1."""
    m = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, model_is_player=1,
        constrained=True, heuristic_temperature=0.0,
    )
    assert r["total_plies"] > 0


def test_constrained_eval_free_mode():
    """Free eval (constrained=False) should also complete."""
    m = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, model_is_player=0,
        constrained=False, heuristic_temperature=0.0,
    )
    assert r["total_plies"] > 0


def test_constrained_eval_with_temperature():
    """Heuristic temperature > 0 should produce results."""
    m = get_test_model("quoridor")
    r = dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id="quoridor", seed=42, model_path=m,
        simulations=10, model_is_player=0,
        constrained=True, heuristic_temperature=1.0,
    )
    assert r["total_plies"] > 0


# ---------------------------------------------------------------------------
# Reject empty model_path in eval/arena
# ---------------------------------------------------------------------------

def test_arena_rejects_empty_model_path():
    """Arena should throw when given empty model_path."""
    with pytest.raises(ValueError, match="model_paths\\[0\\] must not be empty"):
        dinoboard_engine.run_arena_match(
            game_id="tictactoe", seed=42,
            model_paths=["", ""],
            simulations_list=[10, 10], temperature=0.0,
        )


def test_eval_rejects_empty_model_path():
    """Constrained eval should throw when given empty model_path."""
    with pytest.raises(ValueError, match="model_path must not be empty"):
        dinoboard_engine.run_constrained_eval_vs_heuristic(
            game_id="quoridor", seed=42, model_path="",
            simulations=10, model_is_player=0,
            constrained=True, heuristic_temperature=0.0,
        )


def test_selfplay_rejects_empty_model_path():
    """Selfplay should throw when given empty model_path."""
    with pytest.raises(ValueError, match="model_path must not be empty"):
        dinoboard_engine.run_selfplay_episode(
            game_id="tictactoe", seed=42, model_path="",
            simulations=10, max_game_plies=9,
        )
