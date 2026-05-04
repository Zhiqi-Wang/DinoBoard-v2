"""Regression: IBeliefTracker's serialize() is deterministic.

The stronger "tracker never reads ground-truth hidden state" invariant is
covered by `test_api_belief_matches_selfplay` — that test drives an API
session with an INDEPENDENT seed through the observation stream produced
by selfplay, and asserts the tracker's serialized belief matches at every
ply. That's a real equivalence check.

Historically this file also contained a
`test_tracker_belief_invariant_under_seed_shuffle` that attempted to drive
two differently-seeded sessions through the SAME action-id sequence.
That design didn't work: different seeds produce different public state
(e.g. Splendor's initial tableau, Azul's factories), so the first few
actions legal in session A are usually not legal in session B, and the
test skipped nearly 100% of the time. Deleted — `test_api_belief_matches_selfplay`
covers the same invariant correctly.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT / "platform"))

import dinoboard_engine as engine
from conftest import get_test_model


# Games whose tracker is non-trivial (carries hidden-info belief). Games
# without a tracker (TicTacToe, Quoridor) have nothing to assert here.
TRACKER_GAMES = ["loveletter", "splendor", "azul"]


@pytest.mark.parametrize("game_id", TRACKER_GAMES)
def test_tracker_belief_snapshot_is_serializable(game_id):
    """Tracker's serialize() output must be deterministic across identical
    internal states. The API belief-match tests rely on this."""
    model_path = get_test_model(game_id)

    gs = engine.GameSession(
        game_id, seed=42, model_path=model_path, use_filter=False)
    legal = gs.get_legal_actions()
    if not legal:
        pytest.skip(f"{game_id}: no legal actions at game start")

    # Apply a few actions to put something in the tracker.
    for _ in range(3):
        if gs.is_terminal:
            break
        legal = gs.get_legal_actions()
        if not legal:
            break
        gs.apply_action(legal[0])

    snap = gs.get_belief_snapshot()

    # Call twice — must produce equal dicts. Catches non-determinism like
    # iterating an unordered_set without sorting.
    snap2 = gs.get_belief_snapshot()
    assert snap == snap2, (
        f"[{game_id}] get_belief_snapshot returned different dicts on "
        f"consecutive calls with unchanged tracker state: {snap} vs {snap2}")
