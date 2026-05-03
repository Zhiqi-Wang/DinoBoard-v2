"""Multiplayer variant tests: 3p/4p sessions, selfplay, z_values, arena."""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, get_test_model


MULTIPLAYER_VARIANTS = [
    ("splendor_3p", 3),
    ("splendor_4p", 4),
    ("azul_3p", 3),
    ("azul_4p", 4),
]


# ---------------------------------------------------------------------------
# Registration and basic properties
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_registered(variant, expected_players):
    """All multiplayer variants should be registered."""
    assert variant in dinoboard_engine.available_games()


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_num_players(variant, expected_players):
    """GameSession should report correct num_players."""
    gs = dinoboard_engine.GameSession(variant, seed=42)
    assert gs.num_players == expected_players


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_game_id_property(variant, expected_players):
    """GameSession.game_id should match construction argument."""
    gs = dinoboard_engine.GameSession(variant, seed=42)
    assert gs.game_id == variant


# ---------------------------------------------------------------------------
# Selfplay
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_selfplay_completes(variant, expected_players):
    """Selfplay should complete for all multiplayer variants."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=80,
    )
    assert ep["total_plies"] > 0
    assert len(ep["samples"]) > 0


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_z_values_length(variant, expected_players):
    """z_values should have one entry per player."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=80,
    )
    for s in ep["samples"]:
        z_vals = s.get("z_values", [])
        if z_vals:
            assert len(z_vals) == expected_players, (
                f"z_values has {len(z_vals)} entries, expected {expected_players}"
            )


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_player_ids_valid(variant, expected_players):
    """Sample player IDs should be in [0, num_players)."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=80,
    )
    for s in ep["samples"]:
        assert 0 <= s["player"] < expected_players


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_winner_valid(variant, expected_players):
    """Winner should be -1 (draw) or a valid player index."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=80,
    )
    w = ep["winner"]
    assert w == -1 or (0 <= w < expected_players)


# ---------------------------------------------------------------------------
# Determinism
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_determinism(variant, expected_players):
    """Same seed should produce identical episodes."""
    ep1 = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=999, model_path=get_test_model(variant), simulations=5,
        max_game_plies=50, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
    )
    ep2 = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=999, model_path=get_test_model(variant), simulations=5,
        max_game_plies=50, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
    )
    assert ep1["total_plies"] == ep2["total_plies"]
    assert ep1["winner"] == ep2["winner"]


# ---------------------------------------------------------------------------
# Feature encoding for multiplayer
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_features_correct_dim(variant, expected_players):
    """Features should match the variant's own feature_dim from encoder."""
    info = dinoboard_engine.encode_state(variant, seed=42)
    expected_dim = info["feature_dim"]
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=50,
    )
    for s in ep["samples"]:
        assert len(s["features"]) == expected_dim


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_features_vary(variant, expected_players):
    """Features should change across plies."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=5,
        max_game_plies=80,
    )
    samples = ep["samples"]
    if len(samples) >= 5:
        assert samples[0]["features"] != samples[4]["features"]


# ---------------------------------------------------------------------------
# GameSession interaction for multiplayer
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_session_interaction(variant, expected_players):
    """GameSession should work: create, legal actions, apply action."""
    gs = dinoboard_engine.GameSession(variant, seed=42)
    assert not gs.is_terminal
    assert gs.current_player >= 0
    assert gs.current_player < expected_players
    legal = gs.get_legal_actions()
    assert len(legal) > 0
    gs.apply_action(legal[0])
    # After one action, should still be valid state
    assert gs.current_player >= 0


@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_session_state_dict(variant, expected_players):
    """State dict should include current_player."""
    gs = dinoboard_engine.GameSession(variant, seed=42)
    state = gs.get_state_dict()
    assert isinstance(state, dict)
    assert "current_player" in state


# ---------------------------------------------------------------------------
# 2-player variants should also work
# ---------------------------------------------------------------------------

def test_splendor_2p_alias():
    """splendor_2p should be an alias for splendor."""
    games = dinoboard_engine.available_games()
    assert "splendor" in games
    assert "splendor_2p" in games

    gs1 = dinoboard_engine.GameSession("splendor", seed=42)
    gs2 = dinoboard_engine.GameSession("splendor_2p", seed=42)
    assert gs1.num_players == 2
    assert gs2.num_players == 2


# ---------------------------------------------------------------------------
# Current player rotates through all players
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_all_players_get_turns(variant, expected_players):
    """Over several plies, all players should get at least one turn."""
    gs = dinoboard_engine.GameSession(variant, seed=42)
    players_seen = set()
    for _ in range(expected_players * 3):
        if gs.is_terminal:
            break
        players_seen.add(gs.current_player)
        legal = gs.get_legal_actions()
        gs.apply_action(legal[0])

    assert len(players_seen) == expected_players, (
        f"only saw players {players_seen}, expected {expected_players} players"
    )


# ---------------------------------------------------------------------------
# Z-values sum to zero for multiplayer zero-sum games
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("variant,expected_players", MULTIPLAYER_VARIANTS)
def test_variant_z_values_sum(variant, expected_players):
    """In zero-sum games, z_values should sum to approximately 0."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=variant, seed=42, model_path=get_test_model(variant), simulations=10,
        max_game_plies=100,
    )
    for s in ep["samples"]:
        z_vals = s.get("z_values", [])
        if z_vals and len(z_vals) == expected_players:
            total = sum(z_vals)
            assert abs(total) < 1e-5, (
                f"ply {s['ply']}: z_values sum={total}, expected ~0"
            )
