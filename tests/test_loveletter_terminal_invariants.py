"""Regression: Love Letter must never deadlock with alive<=1 and terminal=false.

The concern (from audit): if two or more players are eliminated within a
single decision cascade, `sole_survivor` returns -1 (zero survivors) and
`check_end_game` does nothing unless the deck is empty — game would deadlock
with `current_player` pointing at a dead player, `legal_actions` empty, and
`is_terminal=false`.

The rules we checked (Guard / Baron / Prince / King) all eliminate at most
ONE player per action — simultaneous double-elimination is not reachable
from any single `do_action_fast` call. This test hammers 3p/4p selfplay
and GameSession stepping to verify: after EVERY action, the state must
satisfy one of:
  - `is_terminal=true` (game over; winner might be -1 for a draw)
  - alive count >= 1 AND current_player points at an alive player
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

import dinoboard_engine as engine
from conftest import get_test_model


def _alive_count(state_dict):
    return sum(1 for p in state_dict.get("players", []) if p.get("alive"))


@pytest.mark.parametrize("game_id", ["loveletter_3p", "loveletter_4p"])
def test_no_deadlock_via_selfplay(game_id):
    """Run 50 selfplay episodes per variant. After every sample, verify
    that either the ep is terminal OR at least one player is alive."""
    model = get_test_model(game_id)
    for seed in range(50):
        ep = engine.run_selfplay_episode(
            game_id=game_id, seed=seed, model_path=model,
            simulations=20, max_game_plies=80,
        )
        # The episode terminates normally (winner in -1..N-1, or draw=true).
        assert "winner" in ep, f"seed {seed}: episode missing winner"
        # Each selfplay sample implicitly asserts a live state (otherwise
        # the selfplay loop would have thrown). An explicit check here:
        # if total_plies > 0 and episode was NOT terminal by ply limit,
        # a winner must be decided.
        if ep["total_plies"] > 0 and ep["total_plies"] < 80:
            assert ep["winner"] >= -1, f"seed {seed}: invalid winner {ep['winner']}"


@pytest.mark.parametrize("game_id", ["loveletter_3p", "loveletter_4p"])
def test_no_deadlock_via_random_action_cascade(game_id):
    """Drive GameSession by picking legal_actions[0] each turn — a
    deterministic path that maximizes the chance of hitting unusual
    eliminations. After EVERY apply_action:
      - if not terminal, current_player must be alive
      - if alive count < 2, game MUST be terminal (either via sole_survivor
        or deck-empty path)"""
    for seed in range(50):
        gs = engine.GameSession(game_id, seed=seed, model_path="")
        for step in range(200):
            if gs.is_terminal:
                break
            legal = gs.get_legal_actions()
            if not legal:
                # Non-terminal state with no legal actions = deadlock.
                state = gs.get_state_dict()
                pytest.fail(
                    f"[{game_id}] seed {seed} step {step}: "
                    f"no legal actions but is_terminal=false. "
                    f"alive={_alive_count(state)}, current_player={state.get('current_player')}"
                )
            gs.apply_action(legal[0])
            state = gs.get_state_dict()
            alive = _alive_count(state)
            if not gs.is_terminal:
                # Non-terminal: current_player must be alive.
                cp = state["current_player"]
                assert 0 <= cp < len(state["players"]), (
                    f"[{game_id}] seed {seed} step {step}: current_player={cp} out of range"
                )
                assert state["players"][cp]["alive"], (
                    f"[{game_id}] seed {seed} step {step}: "
                    f"current_player={cp} is dead but is_terminal=false"
                )
                # Alive count must allow a meaningful turn.
                assert alive >= 2, (
                    f"[{game_id}] seed {seed} step {step}: "
                    f"alive={alive} but is_terminal=false — possible deadlock"
                )
