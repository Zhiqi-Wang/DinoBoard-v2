"""encode_state API completeness: all return fields, cross-game consistency."""
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, GAME_CONFIGS


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_returns_all_fields(game_id):
    """encode_state should return features, legal_mask, legal_actions,
    current_player, is_terminal, action_space, feature_dim."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert "features" in info
    assert "legal_mask" in info
    assert "legal_actions" in info
    assert "current_player" in info
    assert "is_terminal" in info
    assert "action_space" in info
    assert "feature_dim" in info


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_feature_dim_field_matches_length(game_id):
    """feature_dim field should equal len(features)."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert info["feature_dim"] == len(info["features"])


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_action_space_field_matches_mask_length(game_id):
    """action_space field should equal len(legal_mask)."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert info["action_space"] == len(info["legal_mask"])


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_matches_config(game_id):
    """feature_dim and action_space from encode_state should match game.json."""
    cfg = GAME_CONFIGS[game_id]
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert info["feature_dim"] == cfg["feature_dim"]
    assert info["action_space"] == cfg["action_space"]


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_initial_not_terminal(game_id):
    """Initial state should not be terminal."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert info["is_terminal"] is False


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_has_legal_actions(game_id):
    """Initial state should have legal actions."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert len(info["legal_actions"]) > 0


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_legal_actions_match_mask(game_id):
    """Legal actions list should match positions where mask == 1."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    mask_legal = {i for i, m in enumerate(info["legal_mask"]) if m > 0}
    action_set = set(info["legal_actions"])
    assert action_set == mask_legal


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_current_player_valid(game_id):
    """current_player should be a valid player index."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert info["current_player"] >= 0
    num_players = GAME_CONFIGS[game_id]["players"]["max"]
    assert info["current_player"] < num_players


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_features_are_float(game_id):
    """Features should be a list of floats, no NaN/Inf."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    import math
    for i, f in enumerate(info["features"]):
        assert isinstance(f, float), f"feature[{i}] is {type(f)}"
        assert math.isfinite(f), f"feature[{i}] = {f}"


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_mask_binary(game_id):
    """Legal mask should only contain 0.0 and 1.0."""
    info = dinoboard_engine.encode_state(game_id, seed=42)
    for i, m in enumerate(info["legal_mask"]):
        assert m in (0.0, 1.0), f"mask[{i}] = {m}"


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_encode_state_different_seeds_same_dim(game_id):
    """Different seeds should produce same feature/mask dimensions."""
    info1 = dinoboard_engine.encode_state(game_id, seed=1)
    info2 = dinoboard_engine.encode_state(game_id, seed=999)
    assert len(info1["features"]) == len(info2["features"])
    assert len(info1["legal_mask"]) == len(info2["legal_mask"])


def test_encode_state_tictactoe_initial_symmetry():
    """TicTacToe initial position should have all 9 actions legal."""
    info = dinoboard_engine.encode_state("tictactoe", seed=42)
    assert len(info["legal_actions"]) == 9
    assert sum(info["legal_mask"]) == 9.0
