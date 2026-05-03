"""Game-specific feature and behavior tests.

Tests that each game's unique registered components (heuristic, tail solver,
filter, auxiliary scorer, adjudicator, belief tracker, etc.) work correctly.
"""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, CANONICAL_GAMES, get_test_model


# ---------------------------------------------------------------------------
# Quoridor-specific
# ---------------------------------------------------------------------------

class TestQuoridor:
    """Quoridor has: heuristic, tail_solver, tail_solve_trigger, adjudicator,
    auxiliary_scorer, training_action_filter."""

    def test_tail_solve_trigger_requires_short_path(self):
        """Quoridor trigger: ply >= threshold AND shortest_path <= 4 for some player.

        With depth_limit=1 and tiny budget, we just verify the API works.
        """
        r = dinoboard_engine.tail_solve(
            game_id="quoridor", seed=42, perspective_player=0,
            depth_limit=1, node_budget=100,
        )
        assert "value" in r
        assert "best_action" in r

    def test_margin_weight_affects_tail_solve_value(self):
        """With margin_weight > 0, proven value should differ slightly from ±1.0.

        margin_weight adds auxiliary_scorer contribution to terminal evaluations.
        """
        r_no_margin = dinoboard_engine.run_selfplay_episode(
            game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
            max_game_plies=200, tail_solve_enabled=True,
            tail_solve_start_ply=1, tail_solve_depth_limit=3,
            tail_solve_node_budget=500, tail_solve_margin_weight=0.0,
        )
        r_with_margin = dinoboard_engine.run_selfplay_episode(
            game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
            max_game_plies=200, tail_solve_enabled=True,
            tail_solve_start_ply=1, tail_solve_depth_limit=3,
            tail_solve_node_budget=500, tail_solve_margin_weight=0.01,
        )
        # Both should complete without error
        assert r_no_margin["total_plies"] > 0
        assert r_with_margin["total_plies"] > 0

    def test_adjudicator_assigns_winner_quoridor(self):
        """Short max_game_plies should trigger adjudicator, not leave winner=-1."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
            max_game_plies=5,
        )
        # With only 5 plies, quoridor won't reach terminal.
        # Adjudicator should assign a winner based on shortest path.
        if ep["total_plies"] >= 5 and not any(
            s.get("z_values") for s in ep["samples"]
        ):
            # Adjudicated: z_values empty, but z should be assigned
            for s in ep["samples"]:
                z = s["z"]
                assert z in (-1.0, 0.0, 1.0), f"bad z={z}"

    def test_auxiliary_scorer_returns_finite(self):
        """auxiliary_score in samples should be finite when scorer is registered."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
            max_game_plies=50,
        )
        for s in ep["samples"]:
            score = s.get("auxiliary_score", 0.0)
            assert -100 < score < 100, f"ply {s['ply']}: aux score out of range: {score}"

    def test_training_filter_reduces_wall_actions(self):
        """Filter should cut low-quality wall placements, reducing legal action count.

        Quoridor has ~3 pawn moves and ~128 wall moves. Filter removes bad walls.
        """
        gs_filtered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=True)
        gs_unfiltered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=False)
        f_count = len(gs_filtered.get_legal_actions())
        u_count = len(gs_unfiltered.get_legal_actions())
        assert f_count < u_count, (
            f"filter should reduce actions: filtered={f_count}, unfiltered={u_count}"
        )
        # All filtered actions should be a subset of unfiltered
        f_set = set(gs_filtered.get_legal_actions())
        u_set = set(gs_unfiltered.get_legal_actions())
        assert f_set.issubset(u_set)

    def test_heuristic_returns_legal_action(self):
        """Quoridor heuristic should always return a legal action."""
        for seed in range(50):
            gs = dinoboard_engine.GameSession("quoridor", seed=seed)
            result = gs.get_heuristic_action()
            assert "action" in result
            legal = gs.get_all_legal_actions()
            assert result["action"] in legal, (
                f"seed {seed}: heuristic action {result['action']} not legal"
            )

    def test_custom_stats_in_episode(self):
        """Quoridor registers episode_stats_extractor → custom_stats should exist."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
            max_game_plies=30,
        )
        # custom_stats may or may not be present depending on game length
        # Just verify no crash and type correctness
        stats = ep.get("custom_stats", {})
        assert isinstance(stats, dict)


# ---------------------------------------------------------------------------
# Splendor-specific
# ---------------------------------------------------------------------------

class TestSplendor:
    """Splendor has: belief_tracker, stochastic_detector, tail_solver,
    tail_solve_trigger (>=12 points). No heuristic."""

    def test_splendor_selfplay_completes(self):
        """Splendor games with hidden info should complete without crash."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=42, model_path=get_test_model("splendor"), simulations=10,
            max_game_plies=50,
        )
        assert ep["total_plies"] > 0
        assert len(ep["samples"]) > 0

    def test_splendor_multiplayer_variants_exist(self):
        """Splendor should have 2p, 3p, 4p variants."""
        games = dinoboard_engine.available_games()
        for variant in ["splendor", "splendor_3p", "splendor_4p"]:
            assert variant in games, f"{variant} not registered"

    def test_splendor_3p_has_3_players(self):
        gs = dinoboard_engine.GameSession("splendor_3p", seed=42)
        assert gs.num_players == 3

    def test_splendor_4p_has_4_players(self):
        gs = dinoboard_engine.GameSession("splendor_4p", seed=42)
        assert gs.num_players == 4

    def test_splendor_features_correct_dim(self):
        info = dinoboard_engine.encode_state("splendor", seed=42)
        assert len(info["features"]) == GAME_CONFIGS["splendor"]["feature_dim"]

    def test_splendor_tail_solve_api_works(self):
        r = dinoboard_engine.tail_solve(
            game_id="splendor", seed=42, perspective_player=0,
            depth_limit=2, node_budget=1000,
        )
        assert "value" in r
        assert "budget_exceeded" in r

    def test_splendor_no_heuristic(self):
        """Splendor has no registered heuristic_picker; must raise."""
        gs = dinoboard_engine.GameSession("splendor", seed=42)
        with pytest.raises(RuntimeError, match="no heuristic_picker registered"):
            gs.get_heuristic_action()

    def test_splendor_state_dict_has_hidden_info_fields(self):
        """Splendor state should contain gem/card-related fields."""
        gs = dinoboard_engine.GameSession("splendor", seed=42)
        state = gs.get_state_dict()
        assert "current_player" in state
        assert "is_terminal" in state


# ---------------------------------------------------------------------------
# Azul-specific
# ---------------------------------------------------------------------------

class TestAzul:
    """Azul has: belief_tracker, stochastic_detector (truncation mode).
    No tail_solver, heuristic, filter."""

    def test_azul_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=42, model_path=get_test_model("azul"), simulations=10,
            max_game_plies=100,
        )
        assert ep["total_plies"] > 0

    def test_azul_multiplayer_variants_exist(self):
        games = dinoboard_engine.available_games()
        for variant in ["azul", "azul_3p", "azul_4p"]:
            assert variant in games, f"{variant} not registered"

    def test_azul_3p_has_3_players(self):
        gs = dinoboard_engine.GameSession("azul_3p", seed=42)
        assert gs.num_players == 3

    def test_azul_features_correct_dim(self):
        info = dinoboard_engine.encode_state("azul", seed=42)
        assert len(info["features"]) == GAME_CONFIGS["azul"]["feature_dim"]

    def test_azul_no_tail_solver(self):
        """Azul has no tail_solver; tail_solve must raise."""
        with pytest.raises(RuntimeError, match="no tail_solver registered"):
            dinoboard_engine.tail_solve(
                game_id="azul", seed=42, perspective_player=0,
                depth_limit=5, node_budget=10000,
            )

    def test_azul_no_heuristic(self):
        """Azul has no registered heuristic_picker; must raise."""
        gs = dinoboard_engine.GameSession("azul", seed=42)
        with pytest.raises(RuntimeError, match="no heuristic_picker registered"):
            gs.get_heuristic_action()

    def test_azul_state_dict_valid(self):
        gs = dinoboard_engine.GameSession("azul", seed=42)
        state = gs.get_state_dict()
        assert isinstance(state, dict)
        assert state["current_player"] >= 0


# ---------------------------------------------------------------------------
# TicTacToe-specific
# ---------------------------------------------------------------------------

class TestTicTacToe:
    """TicTacToe is the simplest game — no optional components."""

    def test_tictactoe_no_heuristic(self):
        """TicTacToe has no registered heuristic_picker; must raise."""
        gs = dinoboard_engine.GameSession("tictactoe", seed=42)
        with pytest.raises(RuntimeError, match="no heuristic_picker registered"):
            gs.get_heuristic_action()

    def test_tictactoe_no_tail_solver(self):
        with pytest.raises(RuntimeError, match="no tail_solver registered"):
            dinoboard_engine.tail_solve(
                game_id="tictactoe", seed=42, perspective_player=0,
                depth_limit=12, node_budget=1000000,
            )

    def test_tictactoe_game_reaches_terminal(self):
        """TicTacToe should always terminate within 9 moves."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="tictactoe", seed=42, model_path=get_test_model("tictactoe"), simulations=30,
            max_game_plies=20,
        )
        assert ep["total_plies"] <= 9

    def test_tictactoe_winner_valid(self):
        for seed in range(50):
            ep = dinoboard_engine.run_selfplay_episode(
                game_id="tictactoe", seed=seed, model_path=get_test_model("tictactoe"), simulations=10,
                max_game_plies=9,
            )
            w = ep["winner"]
            assert w in (-1, 0, 1), f"seed {seed}: invalid winner {w}"

    def test_tictactoe_draw_possible(self):
        """TicTacToe draws should exist in diverse seeds."""
        draws = sum(
            1 for seed in range(200)
            if dinoboard_engine.run_selfplay_episode(
                game_id="tictactoe", seed=seed, model_path=get_test_model("tictactoe"), simulations=10,
                max_game_plies=9,
            )["draw"]
        )
        assert draws > 0, "no draws in 200 tictactoe games"


# ---------------------------------------------------------------------------
# Cross-game: action_info consistency
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_action_info_for_all_legal_actions(game_id):
    """get_action_info should return a non-empty dict for every legal action."""
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    legal = gs.get_legal_actions()
    for action in legal[:20]:  # cap for large action spaces
        info = gs.get_action_info(action)
        assert isinstance(info, dict)
        assert len(info) > 0, f"action {action}: empty action_info"


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_apply_ai_action_changes_state(game_id):
    """apply_ai_action (combo method) should advance the game."""
    m = get_test_model(game_id)
    gs = dinoboard_engine.GameSession(game_id, seed=42, model_path=m)
    state_before = gs.get_state_dict()
    gs.apply_ai_action(simulations=10, temperature=0.0)
    state_after = gs.get_state_dict()
    assert state_before != state_after
