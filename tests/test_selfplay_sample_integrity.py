"""Structural validity of selfplay episode output."""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, run_short_selfplay


def test_sample_features_correct_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    expected = game_config["feature_dim"]
    for s in ep["samples"]:
        assert len(s["features"]) == expected


def test_sample_legal_mask_correct_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    expected = game_config["action_space"]
    for s in ep["samples"]:
        assert len(s["legal_mask"]) == expected


def test_sample_z_values_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    num_players = game_config["players"]["min"]
    for s in ep["samples"]:
        z_vals = s["z_values"]
        if len(z_vals) > 0:
            assert len(z_vals) == num_players, (
                f"ply {s['ply']}: z_values len={len(z_vals)}, expected {num_players}"
            )


def test_policy_ids_and_visits_same_length(game_id, game_config):
    ep = run_short_selfplay(game_id)
    for s in ep["samples"]:
        assert len(s["policy_action_ids"]) == len(s["policy_action_visits"]), (
            f"ply {s['ply']}: ids={len(s['policy_action_ids'])}, visits={len(s['policy_action_visits'])}"
        )


def test_policy_action_ids_in_range(game_id, game_config):
    ep = run_short_selfplay(game_id)
    action_space = game_config["action_space"]
    for s in ep["samples"]:
        for aid in s["policy_action_ids"]:
            assert 0 <= aid < action_space, f"ply {s['ply']}: action_id {aid} out of range"


def test_policy_visits_nonnegative(game_id, game_config):
    ep = run_short_selfplay(game_id)
    for s in ep["samples"]:
        for v in s["policy_action_visits"]:
            assert v >= 0, f"ply {s['ply']}: negative visit count {v}"


def test_features_differ_between_plies(game_id, game_config):
    """BUG-007 regression: features must not be identical across plies."""
    ep = run_short_selfplay(game_id)
    samples = ep["samples"]
    if len(samples) < 4:
        pytest.skip("episode too short")
    assert samples[0]["features"] != samples[3]["features"]


def test_sample_ply_monotonic(game_id, game_config):
    ep = run_short_selfplay(game_id)
    plies = [s["ply"] for s in ep["samples"]]
    for i in range(1, len(plies)):
        assert plies[i] >= plies[i - 1], f"ply not monotonic: {plies[i-1]} -> {plies[i]}"


def test_sample_player_valid(game_id, game_config):
    ep = run_short_selfplay(game_id)
    num_players = game_config["players"]["min"]
    for s in ep["samples"]:
        assert 0 <= s["player"] < num_players, f"ply {s['ply']}: player={s['player']}"


def test_chosen_action_in_legal_mask(game_id, game_config):
    ep = run_short_selfplay(game_id)
    for s in ep["samples"]:
        aid = s["action_id"]
        mask = s["legal_mask"]
        if 0 <= aid < len(mask):
            assert mask[aid] == 1.0, f"ply {s['ply']}: action {aid} not in legal mask"


def test_winner_consistent_with_z_values(game_id, game_config):
    ep = run_short_selfplay(game_id)
    winner = ep["winner"]
    if ep["draw"] or winner < 0:
        return
    num_players = game_config["players"]["min"]
    for s in ep["samples"]:
        z_vals = s["z_values"]
        if len(z_vals) != num_players:
            continue
        assert z_vals[winner] == 1.0, (
            f"ply {s['ply']}: winner={winner} but z_values[{winner}]={z_vals[winner]}"
        )
        for p in range(num_players):
            if p != winner:
                assert z_vals[p] <= 0.0, (
                    f"ply {s['ply']}: loser p{p} has z={z_vals[p]}"
                )


def test_tail_solve_stats_invariant(game_id, game_config):
    ep = run_short_selfplay(game_id)
    attempts = ep.get("tail_solve_attempts", 0)
    completed = ep.get("tail_solve_completed", 0)
    successes = ep.get("tail_solve_successes", 0)
    assert successes <= completed <= attempts, (
        f"tail solve invariant violated: {successes} <= {completed} <= {attempts}"
    )
