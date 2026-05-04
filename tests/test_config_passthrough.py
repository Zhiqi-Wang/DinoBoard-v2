"""Verify training config parameters actually reach the C++ engine."""
import json
from pathlib import Path

import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, PROJECT_ROOT, run_short_selfplay, get_test_model


def test_tail_solve_disabled_produces_no_stats_quoridor():
    """When tail_solve_enabled=False, attempts must be 0 regardless of game state."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=200, tail_solve_enabled=False,
    )
    assert ep["tail_solve_attempts"] == 0, "tail solve disabled but got attempts"


def test_tail_solve_enabled_has_valid_stats_quoridor():
    """When enabled, stats invariant holds: successes <= completed <= attempts."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=200, tail_solve_enabled=True, tail_solve_start_ply=1,
        tail_solve_depth_limit=3, tail_solve_node_budget=1000,
    )
    a = ep["tail_solve_attempts"]
    c = ep["tail_solve_completed"]
    s = ep["tail_solve_successes"]
    assert s <= c <= a, f"invariant violated: {s} <= {c} <= {a}"


def test_tail_solve_tiny_budget_has_more_exceeded():
    ep_tiny = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=50, tail_solve_enabled=True, tail_solve_start_ply=1,
        tail_solve_depth_limit=3, tail_solve_node_budget=10,
    )
    ep_large = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=50, tail_solve_enabled=True, tail_solve_start_ply=1,
        tail_solve_depth_limit=10, tail_solve_node_budget=200000,
    )
    tiny_ratio = ep_tiny["tail_solve_successes"] / max(1, ep_tiny["tail_solve_attempts"])
    large_ratio = ep_large["tail_solve_successes"] / max(1, ep_large["tail_solve_attempts"])
    assert large_ratio >= tiny_ratio, (
        f"larger budget should succeed more: tiny={tiny_ratio:.2f}, large={large_ratio:.2f}"
    )


def test_heuristic_guidance_ratio_injects_heuristic_quoridor():
    ep_full = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=100, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=30, heuristic_guidance_ratio=1.0, heuristic_temperature=0.0,
    )
    ep_none = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=100, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=30, heuristic_guidance_ratio=0.0,
    )
    full_visits = [sum(s["policy_action_visits"]) for s in ep_full["samples"]]
    none_visits = [sum(s["policy_action_visits"]) for s in ep_none["samples"]]
    avg_full = sum(full_visits) / max(1, len(full_visits))
    avg_none = sum(none_visits) / max(1, len(none_visits))
    assert avg_full != avg_none, (
        "heuristic_guidance_ratio=1.0 and 0.0 produce identical visit patterns"
    )


def test_training_filter_ratio_restricts_policy_not_mask_quoridor():
    ep_filtered = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=30, training_filter_ratio=1.0,
    )
    ep_unfiltered = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=30, training_filter_ratio=0.0,
    )
    # legal_mask should always reflect full action space (not restricted by filter)
    filtered_mask_sums = [sum(s["legal_mask"]) for s in ep_filtered["samples"]]
    unfiltered_mask_sums = [sum(s["legal_mask"]) for s in ep_unfiltered["samples"]]
    avg_filtered_mask = sum(filtered_mask_sums) / max(1, len(filtered_mask_sums))
    avg_unfiltered_mask = sum(unfiltered_mask_sums) / max(1, len(unfiltered_mask_sums))
    assert avg_filtered_mask >= avg_unfiltered_mask, (
        f"legal_mask should not be restricted by filter: filtered={avg_filtered_mask:.1f}, unfiltered={avg_unfiltered_mask:.1f}"
    )
    # policy actions (visits) should be restricted when filter is active
    filtered_policy_counts = [len(s["policy_action_ids"]) for s in ep_filtered["samples"]]
    unfiltered_policy_counts = [len(s["policy_action_ids"]) for s in ep_unfiltered["samples"]]
    avg_filtered_policy = sum(filtered_policy_counts) / max(1, len(filtered_policy_counts))
    avg_unfiltered_policy = sum(unfiltered_policy_counts) / max(1, len(unfiltered_policy_counts))
    assert avg_filtered_policy < avg_unfiltered_policy, (
        f"filter should restrict policy actions: filtered={avg_filtered_policy:.1f}, unfiltered={avg_unfiltered_policy:.1f}"
    )


def test_max_game_plies_limits_episode(game_id, game_config):
    max_plies = 10
    ep = run_short_selfplay(game_id, max_game_plies=max_plies)
    assert ep["total_plies"] <= max_plies, (
        f"total_plies={ep['total_plies']} > max_game_plies={max_plies}"
    )


def test_splendor_nested_temperature_schedule_read_by_pipeline():
    """Pipeline reads Splendor's nested temperature_schedule correctly."""
    from training.pipeline import _get_temperature_key
    cfg = GAME_CONFIGS["splendor"]["training"]
    assert "temperature_schedule" in cfg
    assert cfg.get("temperature_initial") is None, "Splendor uses nested, not flat keys"
    assert _get_temperature_key(cfg, "initial", -1.0) == 1.0
    assert _get_temperature_key(cfg, "final", -1.0) == 0.1
    assert _get_temperature_key(cfg, "decay_plies", 0) == 30


def test_all_game_json_training_keys_are_well_formed(game_id, game_config):
    """Pipeline-consumed keys must be either flat values or recognized
    nested shapes. BUG-008 fix allowed `temperature_schedule` nested dict
    alongside the flat `temperature_{initial,final,decay_plies}` keys —
    `_get_temperature_key` in pipeline handles both.

    We check:
      1. temperature keys: must be EITHER flat OR under `temperature_schedule`
         — not some other nested shape.
      2. All OTHER pipeline keys must be flat (unexpected nesting = typo).
    """
    cfg = game_config.get("training", {})

    # Temperature keys: flat OR nested under `temperature_schedule`.
    temperature_keys = {"temperature_initial", "temperature_final", "temperature_decay_plies"}
    sched = cfg.get("temperature_schedule")
    has_nested_sched = isinstance(sched, dict)
    for k in temperature_keys:
        val = cfg.get(k)
        if val is not None and isinstance(val, dict):
            pytest.fail(
                f"{game_id}: temperature key '{k}' is nested (type={type(val).__name__}); "
                f"must be a scalar or placed under 'temperature_schedule'")
    if has_nested_sched:
        # Verify the nested shape has the right structure.
        for nk in ("initial", "final", "decay_plies"):
            if nk in sched:
                assert not isinstance(sched[nk], dict), (
                    f"{game_id}: temperature_schedule.{nk} is doubly-nested")

    # All OTHER pipeline keys must be flat.
    other_keys = [
        "simulations", "c_puct", "temperature",
        "dirichlet_alpha", "dirichlet_epsilon", "dirichlet_on_first_n_plies",
        "max_game_plies", "tail_solve_enabled", "tail_solve_start_ply",
        "tail_solve_depth_limit", "tail_solve_node_budget", "tail_solve_margin_weight",
        "heuristic_guidance_ratio", "heuristic_temperature", "training_filter_ratio",
    ]
    nested_keys = []
    for key in other_keys:
        val = cfg.get(key)
        if isinstance(val, dict):
            nested_keys.append(key)
    assert not nested_keys, (
        f"{game_id}: unexpected nested dicts for pipeline keys: {nested_keys}")


def test_simulations_affects_visit_count():
    ep_low = dinoboard_engine.run_selfplay_episode(
        game_id="tictactoe", seed=42, model_path=get_test_model("tictactoe"), simulations=5, max_game_plies=9,
    )
    ep_high = dinoboard_engine.run_selfplay_episode(
        game_id="tictactoe", seed=42, model_path=get_test_model("tictactoe"), simulations=50, max_game_plies=9,
    )
    if not ep_low["samples"] or not ep_high["samples"]:
        pytest.skip("no samples")
    visits_low = sum(ep_low["samples"][0]["policy_action_visits"])
    visits_high = sum(ep_high["samples"][0]["policy_action_visits"])
    assert visits_high > visits_low, (
        f"more sims should yield more visits: low={visits_low}, high={visits_high}"
    )
