"""Cross-cutting smoke tests for the engine registry."""
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, get_test_model


def test_available_games_contains_all_four():
    games = dinoboard_engine.available_games()
    for g in CANONICAL_GAMES:
        assert g in games, f"{g} not in available_games: {games}"


def test_encode_state_works(game_id):
    enc = dinoboard_engine.encode_state(game_id)
    for key in ["features", "legal_mask", "legal_actions", "current_player",
                "is_terminal", "action_space", "feature_dim"]:
        assert key in enc, f"missing key: {key}"
    assert isinstance(enc["features"], list)
    assert isinstance(enc["legal_mask"], list)
    assert isinstance(enc["legal_actions"], list)


def test_selfplay_completes(game_id):
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id), simulations=5, max_game_plies=30,
    )
    assert "winner" in ep
    assert "samples" in ep
    assert isinstance(ep["samples"], list)


def test_game_session_creation(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    assert gs.game_id == game_id
    assert gs.num_players >= 2
    assert isinstance(gs.is_terminal, bool)
    assert isinstance(gs.current_player, int)


def test_multiplayer_variants_exist():
    games = set(dinoboard_engine.available_games())
    for base in ["splendor", "azul"]:
        for suffix in ["_2p", "_3p", "_4p"]:
            variant = base + suffix
            assert variant in games, f"{variant} not registered"
