"""Tests for the web platform GameSession bindings."""
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, get_test_model


def test_state_dict_returns_dict(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    state = gs.get_state_dict()
    assert isinstance(state, dict)
    assert "current_player" in state
    assert "is_terminal" in state


def test_legal_actions_nonempty_at_start(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    legal = gs.get_legal_actions()
    assert len(legal) > 0


def test_use_filter_restricts_actions_quoridor():
    gs_filtered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=True)
    gs_unfiltered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=False)
    filtered = gs_filtered.get_legal_actions()
    unfiltered = gs_unfiltered.get_legal_actions()
    assert len(filtered) <= len(unfiltered), (
        f"filtered={len(filtered)} > unfiltered={len(unfiltered)}"
    )


def test_all_legal_actions_ignores_filter_quoridor():
    gs_filtered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=True)
    gs_unfiltered = dinoboard_engine.GameSession("quoridor", seed=42, use_filter=False)
    all_filtered = gs_filtered.get_all_legal_actions()
    all_unfiltered = gs_unfiltered.get_all_legal_actions()
    assert all_filtered == all_unfiltered, "get_all_legal_actions should ignore filter"


def test_apply_action_changes_state(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    state_before = gs.get_state_dict()
    legal = gs.get_legal_actions()
    gs.apply_action(legal[0])
    state_after = gs.get_state_dict()
    assert state_before != state_after


def test_properties_correct_types(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    assert isinstance(gs.is_terminal, bool)
    assert isinstance(gs.current_player, int)
    assert isinstance(gs.winner, int)
    assert isinstance(gs.num_players, int)
    assert isinstance(gs.game_id, str)
    assert gs.game_id == game_id
    assert gs.num_players >= 2


def test_ai_action_is_legal(game_id):
    model = get_test_model(game_id)
    gs = dinoboard_engine.GameSession(game_id, seed=42, model_path=model)
    result = gs.get_ai_action(simulations=5, temperature=0.0)
    assert "action" in result
    legal = gs.get_all_legal_actions()
    assert result["action"] in legal, f"AI action {result['action']} not legal"


def test_heuristic_action_is_legal_quoridor():
    gs = dinoboard_engine.GameSession("quoridor", seed=42)
    result = gs.get_heuristic_action()
    assert "action" in result
    legal = gs.get_all_legal_actions()
    assert result["action"] in legal


def test_action_info_returns_dict(game_id):
    gs = dinoboard_engine.GameSession(game_id, seed=42)
    legal = gs.get_legal_actions()
    info = gs.get_action_info(legal[0])
    assert isinstance(info, dict)
    assert len(info) > 0
