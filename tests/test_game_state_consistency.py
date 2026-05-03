"""Game state invariants: terminal conditions, legal actions, state transitions."""
import random

import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES


def test_game_session_initial_not_terminal(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    assert not gs.is_terminal, f"{game_id} starts terminal"


def test_game_session_has_legal_actions_when_not_terminal(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    assert not gs.is_terminal
    legal = gs.get_legal_actions()
    assert len(legal) > 0, f"{game_id} has no legal actions at start"


def test_apply_action_changes_state(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    state_before = gs.get_state_dict()
    legal = gs.get_legal_actions()
    gs.apply_action(legal[0])
    state_after = gs.get_state_dict()
    assert state_before != state_after, "state unchanged after action"


def test_random_game_reaches_terminal_or_max_plies(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    rng = random.Random(42)
    plies = 0
    max_plies = 500
    while not gs.is_terminal and plies < max_plies:
        legal = gs.get_legal_actions()
        if not legal:
            break
        gs.apply_action(rng.choice(legal))
        plies += 1
    assert plies > 0, "no moves made"


def test_terminal_has_no_legal_actions(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    rng = random.Random(42)
    plies = 0
    while not gs.is_terminal and plies < 500:
        legal = gs.get_legal_actions()
        if not legal:
            break
        gs.apply_action(rng.choice(legal))
        plies += 1
    if gs.is_terminal:
        legal = gs.get_legal_actions()
        assert len(legal) == 0, f"terminal state has {len(legal)} legal actions"


def test_winner_is_valid(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    rng = random.Random(42)
    plies = 0
    while not gs.is_terminal and plies < 500:
        legal = gs.get_legal_actions()
        if not legal:
            break
        gs.apply_action(rng.choice(legal))
        plies += 1
    if gs.is_terminal:
        w = gs.winner
        assert w == -1 or (0 <= w < gs.num_players), f"invalid winner: {w}"


def test_current_player_alternates_tictactoe():
    gs = dinoboard_engine.GameSession("tictactoe", seed=42)
    expected_player = 0
    plies = 0
    while not gs.is_terminal and plies < 9:
        assert gs.current_player == expected_player, (
            f"ply {plies}: expected p{expected_player}, got p{gs.current_player}"
        )
        legal = gs.get_legal_actions()
        gs.apply_action(legal[0])
        expected_player = 1 - expected_player
        plies += 1
