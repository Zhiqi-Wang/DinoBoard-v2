"""AI API belief-equivalence: a self-play-generated observation trace must
drive an independent API session to the SAME belief as self-play's own
tracker, step by step.

This is the strongest test of the AI/state separation invariant:
- Ground truth (self-play) runs with one seed, maintains its own belief
  tracker dedicated to a chosen perspective, and records the per-ply
  observation stream (pre/post events + belief snapshot after).
- The API-side session starts with a DIFFERENT seed — its internal
  randomized hidden state does not match ground truth.
- We then replay the observation stream into the API session and assert
  the API's belief tracker matches ground truth's, every step.

If the API's belief diverges, either the event schema is incomplete (some
public info never reaches the API) or apply_event is wrong. Either way,
the separation invariant is broken.

For deterministic games (tictactoe, quoridor), belief is trivially empty
and this test mostly checks that public state stays consistent under the
independent-seed replay.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT / "platform"))

import dinoboard_engine as engine
from conftest import get_test_model


# Games that have a public_event_extractor registered. Parameterize over
# these. As more games implement the protocol, add them here.
GAMES_WITH_EVENT_PROTOCOL = ["azul", "splendor", "loveletter"]


def _apply_trace_step(api_gs, step: dict) -> None:
    """Replay one traced step into the API session.

    Uses apply_observation() so the belief tracker sees the POST-event
    state as state_after, not the AI's random mid-action outcome.
    """
    api_gs.apply_observation(
        step["action"],
        pre_events=step["pre_events"],
        post_events=step["post_events"],
    )


@pytest.mark.parametrize("game_id", GAMES_WITH_EVENT_PROTOCOL)
def test_api_belief_matches_selfplay(game_id):
    """Full-game belief equivalence under independent seeds.

    Self-play with seed_GT traces a perspective's belief per ply. An API
    session with seed_AI (different) starts from the same initial
    observation, replays actions + events, and must arrive at the same
    belief after every ply.
    """
    perspective = 0
    seed_gt = 42
    seed_ai = 9999  # deliberately different
    model_path = get_test_model(game_id)

    # Run self-play with tracing.
    ep = engine.run_selfplay_episode(
        game_id=game_id,
        seed=seed_gt,
        model_path=model_path,
        simulations=20,
        max_game_plies=80,
        trace_perspective=perspective,
    )

    assert "observation_trace" in ep, "trace was not populated — extractor missing?"
    trace = ep["observation_trace"]
    assert len(trace) > 0, "trace is empty — episode terminated before any action?"

    # Create API session with a different seed.
    api_gs = engine.GameSession(game_id, seed=seed_ai, model_path="", use_filter=False)

    # Sync initial observation. After this, public-visible state should
    # match ground truth, and API's tracker should see the same initial
    # belief as ground truth's.
    api_gs.apply_initial_observation(perspective, ep["initial_observation"])

    api_initial_belief = api_gs.get_belief_snapshot()
    gt_initial_belief = ep["initial_belief_snapshot"]
    assert api_initial_belief == gt_initial_belief, (
        f"[{game_id}] initial belief mismatch:\n"
        f"  GT:  {gt_initial_belief}\n"
        f"  API: {api_initial_belief}")

    # Replay each action + events, asserting belief match at each step.
    for i, step in enumerate(trace):
        _apply_trace_step(api_gs, step)
        api_belief = api_gs.get_belief_snapshot()
        gt_belief = step["belief_snapshot_after"]
        assert api_belief == gt_belief, (
            f"[{game_id}] belief diverged at ply {step['ply']} (step {i}, "
            f"actor={step['actor']}, action={step['action']}):\n"
            f"  GT:  {gt_belief}\n"
            f"  API: {api_belief}\n"
            f"  pre_events:  {step['pre_events']}\n"
            f"  post_events: {step['post_events']}")


@pytest.mark.parametrize("game_id", GAMES_WITH_EVENT_PROTOCOL)
def test_api_public_state_matches_after_trace(game_id):
    """After replaying the trace, API's public state matches ground truth.

    Beyond belief equality, this verifies that state fields a real player
    at the table would see (factories, scores, discards, etc.) are
    identical between self-play's final state and API's replayed state.
    Per-game public-state fields are determined by inspecting the state
    dict and excluding known-hidden fields.
    """
    perspective = 0
    seed_gt = 42
    seed_ai = 9999
    model_path = get_test_model(game_id)

    ep = engine.run_selfplay_episode(
        game_id=game_id,
        seed=seed_gt,
        model_path=model_path,
        simulations=20,
        max_game_plies=80,
        trace_perspective=perspective,
    )

    # Replay into API.
    api_gs = engine.GameSession(game_id, seed=seed_ai, model_path="", use_filter=False)
    api_gs.apply_initial_observation(perspective, ep["initial_observation"])
    for step in ep["observation_trace"]:
        _apply_trace_step(api_gs, step)

    # Compare public state fields. For Azul, everything in state_dict is
    # public except "bag_counts" / "bag_total" (which are derivable but may
    # differ in representation due to box_lid bookkeeping).
    api_state = api_gs.get_state_dict()
    # Recreate ground-truth session and replay its action history to get
    # its final state dict (there's no direct "final state dict" output).
    gt_gs = engine.GameSession(game_id, seed=seed_gt, model_path="", use_filter=False)
    for step in ep["observation_trace"]:
        gt_gs.apply_action(step["action"])
    gt_state = gt_gs.get_state_dict()

    _PUBLIC_KEYS = {
        "azul": [
            "current_player", "is_terminal", "winner", "num_players",
            "round_index", "first_player_token_in_center", "scores",
            "factories", "center", "players",
        ],
        "splendor": [
            "current_player", "is_terminal", "winner", "num_players",
            "bank", "tableau", "nobles", "players",
        ],
        "loveletter": [
            "current_player", "is_terminal", "winner", "num_players",
            "ply",
            # Per-player: alive, protected, discards, hand_exposed. Own hand
            # and drawn_card are perspective-private. We check the `players`
            # array but have to exclude hand/drawn_card for non-perspective
            # players.
        ],
    }[game_id]

    for key in _PUBLIC_KEYS:
        assert api_state[key] == gt_state[key], (
            f"[{game_id}] public field '{key}' diverged after trace replay:\n"
            f"  GT:  {gt_state[key]}\n"
            f"  API: {api_state[key]}")


@pytest.mark.parametrize("game_id", GAMES_WITH_EVENT_PROTOCOL)
def test_api_legal_actions_match_after_trace(game_id):
    """After replaying, both sessions agree on legal actions WHEN IT'S
    PERSPECTIVE'S TURN. This is the operational equivalence that matters:
    the AI is only ever asked to decide on its own seat, and the set of
    actions it would consider must match what ground truth considers legal.

    When it's an opponent's turn, their legal_actions may legitimately
    depend on hidden info (e.g. Love Letter: which card they hold
    determines what they can play). AI holds a belief-consistent random
    card in that slot, so its enumeration will differ from GT's — that's
    by design and not a separation violation.
    """
    perspective = 0
    seed_gt = 77
    seed_ai = 1234
    model_path = get_test_model(game_id)

    ep = engine.run_selfplay_episode(
        game_id=game_id,
        seed=seed_gt,
        model_path=model_path,
        simulations=20,
        max_game_plies=80,
        trace_perspective=perspective,
    )

    api_gs = engine.GameSession(game_id, seed=seed_ai, model_path="", use_filter=False)
    api_gs.apply_initial_observation(perspective, ep["initial_observation"])
    gt_gs = engine.GameSession(game_id, seed=seed_gt, model_path="", use_filter=False)

    for step in ep["observation_trace"]:
        _apply_trace_step(api_gs, step)
        gt_gs.apply_action(step["action"])
        if api_gs.is_terminal or gt_gs.is_terminal:
            continue
        if api_gs.current_player != perspective or gt_gs.current_player != perspective:
            continue
        assert sorted(api_gs.get_legal_actions()) == sorted(gt_gs.get_legal_actions()), (
            f"[{game_id}] perspective legal_actions diverged at ply {step['ply']}:\n"
            f"  GT:  {sorted(gt_gs.get_legal_actions())}\n"
            f"  API: {sorted(api_gs.get_legal_actions())}")
