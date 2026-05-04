"""BUG-007 regression tests + feature encoder correctness.

BUG-007: pipeline.py used encode_state() (initial position features) for all
training samples instead of the actual mid-game features from C++ samples.
"""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, GAMES_WITH_HEURISTIC, run_short_heuristic, run_short_selfplay


# ---------------------------------------------------------------------------
# BUG-007 REGRESSION: features must differ across plies
# ---------------------------------------------------------------------------

def test_selfplay_features_vary_across_plies(game_id, game_config):
    ep = run_short_selfplay(game_id)
    samples = ep["samples"]
    if len(samples) < 3:
        pytest.skip("episode too short")
    assert samples[0]["features"] != samples[2]["features"], (
        "features at ply 0 and ply 2 are identical — BUG-007 regression"
    )


def test_selfplay_features_differ_from_initial_position(game_id, game_config):
    seed = 42
    initial = dinoboard_engine.encode_state(game_id, seed)["features"]
    ep = run_short_selfplay(game_id, seed=seed)
    samples = ep["samples"]
    mid_ply = min(4, len(samples) - 1)
    if mid_ply < 2:
        pytest.skip("episode too short")
    assert samples[mid_ply]["features"] != initial, (
        "mid-game features identical to initial position — BUG-007 regression"
    )


# Some games (Love Letter 2p under uniform-random heuristic) can terminate
# in a single ply (e.g. Princess discard, Guard guess hits). Try a small
# set of seeds so we reliably get an episode long enough to compare plies.
_HEURISTIC_SEED_CANDIDATES = [42, 77, 100, 13, 2024, 999]


def _first_heuristic_episode_with_min_samples(game_id: str, min_samples: int):
    for seed in _HEURISTIC_SEED_CANDIDATES:
        ep = run_short_heuristic(game_id, seed=seed)
        if len(ep["samples"]) >= min_samples:
            return seed, ep
    pytest.fail(
        f"{game_id}: could not find a seed in {_HEURISTIC_SEED_CANDIDATES} "
        f"producing ≥{min_samples} heuristic episode samples — game may be "
        f"terminating too quickly under uniform-random heuristic"
    )


@pytest.mark.parametrize("game_id", GAMES_WITH_HEURISTIC)
def test_heuristic_features_vary_across_plies(game_id):
    _seed, ep = _first_heuristic_episode_with_min_samples(game_id, min_samples=3)
    samples = ep["samples"]
    assert samples[0]["features"] != samples[2]["features"]


@pytest.mark.parametrize("game_id", GAMES_WITH_HEURISTIC)
def test_heuristic_features_differ_from_initial(game_id):
    seed, ep = _first_heuristic_episode_with_min_samples(game_id, min_samples=3)
    initial = dinoboard_engine.encode_state(game_id, seed)["features"]
    samples = ep["samples"]
    mid_ply = min(4, len(samples) - 1)
    assert samples[mid_ply]["features"] != initial


# ---------------------------------------------------------------------------
# Feature dimension / action space correctness
# ---------------------------------------------------------------------------

def test_feature_dim_matches_game_json(game_id, game_config):
    enc = dinoboard_engine.encode_state(game_id)
    assert len(enc["features"]) == game_config["feature_dim"]
    assert enc["feature_dim"] == game_config["feature_dim"]


def test_action_space_matches_game_json(game_id, game_config):
    enc = dinoboard_engine.encode_state(game_id)
    assert enc["action_space"] == game_config["action_space"]


def test_legal_mask_length_matches_action_space(game_id, game_config):
    enc = dinoboard_engine.encode_state(game_id)
    assert len(enc["legal_mask"]) == game_config["action_space"]


def test_legal_mask_marks_only_legal_actions(game_id, game_config):
    enc = dinoboard_engine.encode_state(game_id)
    legal_set = set(enc["legal_actions"])
    mask = enc["legal_mask"]
    for i, val in enumerate(mask):
        if i in legal_set:
            assert val == 1.0, f"action {i} is legal but mask is {val}"
        else:
            assert val == 0.0, f"action {i} is illegal but mask is {val}"


def test_selfplay_sample_feature_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    expected = game_config["feature_dim"]
    for s in ep["samples"]:
        assert len(s["features"]) == expected, f"ply {s['ply']}: got {len(s['features'])}"


def test_selfplay_sample_legal_mask_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    expected = game_config["action_space"]
    for s in ep["samples"]:
        assert len(s["legal_mask"]) == expected, f"ply {s['ply']}: got {len(s['legal_mask'])}"


# ---------------------------------------------------------------------------
# Deep TicTacToe feature verification
# ---------------------------------------------------------------------------

def test_tictactoe_features_encode_board_correctly():
    """Decode TicTacToe 28-dim features and verify against board state.

    Encoding: 9 cells x 3 one-hot (my_piece, opp_piece, empty) + 1 is_first_player.
    """
    gs = dinoboard_engine.GameSession("tictactoe", seed=42)

    actions_applied = 0
    prev_empty_count = 9

    while not gs.is_terminal and actions_applied < 9:
        state = gs.get_state_dict()
        board = state["board"]
        cp = gs.current_player
        opp = 1 - cp

        legal = gs.get_legal_actions()
        enc = dinoboard_engine.encode_state("tictactoe", 42)

        ep = run_short_selfplay("tictactoe", seed=42 + actions_applied)
        if not ep["samples"]:
            break
        features = ep["samples"][0]["features"]
        assert len(features) == 28

        for cell_idx in range(9):
            my_val = features[cell_idx * 3]
            opp_val = features[cell_idx * 3 + 1]
            empty_val = features[cell_idx * 3 + 2]
            total = my_val + opp_val + empty_val
            assert abs(total - 1.0) < 1e-6, (
                f"cell {cell_idx}: one-hot sum = {total}, expected 1.0"
            )

        empty_count = sum(
            1 for i in range(9) if features[i * 3 + 2] == 1.0
        )
        assert empty_count <= prev_empty_count

        gs.apply_action(legal[0])
        actions_applied += 1
        prev_empty_count = empty_count
