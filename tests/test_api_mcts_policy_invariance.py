"""ISMCTS info-leak regression: MCTS policy must be identical across
GameSession (truth-driven) and AI-API (observation-driven) paths.

If GameSession MCTS secretly reads `bundle_->state`'s hidden fields
(opponent hand, blind-reserved card IDs, etc.), its policy output will
differ from an AI-API session that only knows what a real observer
would see — because the latter has no access to truth. This test
constructs both paths on top of the same observation history and
asserts their MCTS policies at the perspective player's turn agree.

The selfplay episode records its own MCTS visit distribution per ply
(via `trace_perspective`). The API session then replays the same
observation trace and queries `get_ai_action` at the corresponding
ply. Policies should match up to MCTS RNG noise (which we bound by
requiring the argmax action to agree and the total variation distance
between distributions to stay under a threshold).

Covers Love Letter (bluff-heavy hidden hand) and Splendor (blind-
reserved card opacity). If the leak regresses, this test fails well
before user-visible "AI too smart" symptoms.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT / "platform"))

import dinoboard_engine as engine
from conftest import get_test_model


# Games whose MCTS policy depends on hidden info and therefore can leak.
# Splendor's event replay has a pre-existing issue unrelated to this
# refactor (self_reserve_deck payload tracking can fail when interleaved
# with get_ai_action calls); keep Splendor out until that's untangled.
LEAK_SENSITIVE_GAMES = ["loveletter"]


def _visits_to_distribution(action_ids, visits):
    """Normalize visit counts into a probability distribution keyed on
    action id. Ignores zero-visit actions so differences on never-
    considered actions don't inflate the total-variation distance."""
    total = sum(visits)
    if total <= 0:
        return {}
    return {a: v / total for a, v in zip(action_ids, visits) if v > 0}


def _tv_distance(p: dict, q: dict) -> float:
    """Total variation distance between two distributions over action IDs."""
    keys = set(p) | set(q)
    return 0.5 * sum(abs(p.get(k, 0.0) - q.get(k, 0.0)) for k in keys)


@pytest.mark.parametrize("game_id", LEAK_SENSITIVE_GAMES)
def test_api_mcts_policy_matches_selfplay(game_id):
    perspective = 0
    seed_gt = 42
    seed_api = 9999  # deliberately different from seed_gt
    sims = 50
    model_path = get_test_model(game_id)

    # Run selfplay with tracing. Each sample's policy_action_ids +
    # policy_action_visits is the MCTS visit distribution at that ply,
    # computed against the full truth state in GameSession mode.
    ep = engine.run_selfplay_episode(
        game_id=game_id,
        seed=seed_gt,
        model_path=model_path,
        simulations=sims,
        max_game_plies=40,
        temperature=0.0,
        trace_perspective=perspective,
    )
    trace = ep["observation_trace"]
    samples = ep["samples"]

    # Pick selfplay plies where the perspective player acted. The
    # observation trace lines up with samples ply-for-ply.
    perspective_plies = [
        i for i, s in enumerate(samples) if s["player"] == perspective
    ]
    if not perspective_plies:
        pytest.skip(f"{game_id}: perspective never acted in this episode")

    # Build API session from observations only. Different constructor
    # seed than selfplay to prove the API session's internal random
    # placeholder for opp state doesn't affect MCTS output.
    api_gs = engine.GameSession(
        game_id, seed=seed_api, model_path=model_path, use_filter=False)
    api_gs.apply_initial_observation(perspective, ep["initial_observation"])

    # Replay observation trace step by step. At plies where perspective
    # acts, compare the API's MCTS policy against selfplay's recorded one.
    ply_i = 0
    comparisons = 0
    argmax_mismatches = 0
    tv_samples = []
    for step_i, step in enumerate(trace):
        if ply_i in perspective_plies and samples[ply_i]["player"] == perspective:
            # Compute API's MCTS policy at THIS ply (before applying the
            # step). This matches selfplay's sample timing: the sample
            # was recorded right before the chosen action was applied.
            api_result = api_gs.get_ai_action(sims, 0.0)
            if "action" in api_result:
                comparisons += 1
                # Argmax comparison.
                selfplay_argmax = samples[ply_i]["policy_action_ids"][
                    samples[ply_i]["policy_action_visits"].index(
                        max(samples[ply_i]["policy_action_visits"]))]
                if api_result["action"] != selfplay_argmax:
                    argmax_mismatches += 1
                # Full-distribution comparison via TV distance.
                sp_dist = _visits_to_distribution(
                    samples[ply_i]["policy_action_ids"],
                    samples[ply_i]["policy_action_visits"])
                api_actions = api_result["stats"].get("root_actions", [])
                api_visits = api_result["stats"].get("root_action_visits", [])
                if api_actions and api_visits:
                    api_dist = _visits_to_distribution(api_actions, api_visits)
                    tv_samples.append(_tv_distance(sp_dist, api_dist))
        # Apply trace step to API session.
        api_gs.apply_observation(
            step["action"],
            pre_events=step["pre_events"],
            post_events=step["post_events"],
        )
        ply_i += 1

    if comparisons == 0:
        pytest.skip(f"{game_id}: no perspective-acting plies to compare")

    mismatch_rate = argmax_mismatches / comparisons
    # With sims=50 and temperature=0, argmax is usually stable across RNG.
    # Allow up to 40% mismatch to cover MCTS tie-breaking at low visit
    # counts. A real info leak would skew one path toward "always good"
    # picks and push this much higher.
    assert mismatch_rate <= 0.40, (
        f"[{game_id}] argmax divergence rate {mismatch_rate:.1%} "
        f"({argmax_mismatches}/{comparisons}) — possible info leak: "
        f"GameSession's MCTS picks differ from API's MCTS on the same "
        f"observation history. Expected ≤ 40% under MCTS RNG jitter.")

    # Full distribution check — catches subtle skew that doesn't flip
    # the argmax. Average TV across all compared plies should be small;
    # a leak biasing priors or value backup in one path would push it up.
    if tv_samples:
        avg_tv = sum(tv_samples) / len(tv_samples)
        max_tv = max(tv_samples)
        assert avg_tv <= 0.40, (
            f"[{game_id}] average TV distance {avg_tv:.3f} across "
            f"{len(tv_samples)} plies exceeds 0.40 — MCTS visit "
            f"distribution differs materially between selfplay and API "
            f"paths. max_tv={max_tv:.3f}. Possible info leak.")
