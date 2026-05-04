"""Shared fixtures and helpers for DinoBoard tests."""
import json
from pathlib import Path

import pytest
import torch

import dinoboard_engine

PROJECT_ROOT = Path(__file__).resolve().parents[1]

CANONICAL_GAMES = ["tictactoe", "quoridor", "splendor", "azul", "loveletter", "coup"]


def load_game_config(game_id: str) -> dict:
    import re
    base = re.sub(r"_\d+p$", "", game_id)
    config_path = PROJECT_ROOT / "games" / base / "config" / "game.json"
    with open(config_path) as f:
        cfg = json.load(f)
    meta = dinoboard_engine.game_metadata(game_id)
    cfg["action_space"] = meta["action_space"]
    cfg["feature_dim"] = meta["feature_dim"]
    cfg["num_players"] = meta["num_players"]
    return cfg


GAME_CONFIGS = {g: load_game_config(g) for g in CANONICAL_GAMES}

_MODEL_CACHE: dict[str, str] = {}


def get_test_model(game_id: str, tmp_path_factory=None) -> str:
    """Return path to a random ONNX model for the given game. Cached per session."""
    if game_id in _MODEL_CACHE:
        return _MODEL_CACHE[game_id]
    from training.model import create_model_from_config, export_onnx
    meta = dinoboard_engine.game_metadata(game_id)
    feature_dim = meta["feature_dim"]
    action_space = meta["action_space"]
    num_players = meta["num_players"]
    cfg = {"feature_dim": feature_dim, "action_space": action_space, "num_players": num_players}
    net = create_model_from_config(cfg)
    model_dir = Path("/tmp/dinoboard_test_models")
    model_dir.mkdir(exist_ok=True)
    path = model_dir / f"test_{game_id}.onnx"
    export_onnx(net, path, feature_dim)
    _MODEL_CACHE[game_id] = str(path)
    return str(path)


@pytest.fixture
def model_path(game_id):
    """Fixture providing a random ONNX model path for the current game_id."""
    return get_test_model(game_id)

# Every registered game has a heuristic_picker — "heuristic" difficulty in the
# web UI requires one, and a uniform-random picker is the no-effort default.
# Quoridor's picker is a real heuristic (pawn-advance + wall-block); the others
# are uniform-random fallbacks.
GAMES_WITH_HEURISTIC = ["tictactoe", "quoridor", "splendor", "azul", "loveletter", "coup"]
GAMES_WITH_TAIL_SOLVER = ["quoridor", "splendor"]
GAMES_WITH_TRAINING_FILTER = ["quoridor"]


@pytest.fixture(params=CANONICAL_GAMES)
def game_id(request):
    return request.param


@pytest.fixture
def game_config(game_id):
    return GAME_CONFIGS[game_id]


def run_short_selfplay(game_id: str, seed: int = 42, **kwargs) -> dict:
    defaults = dict(
        simulations=10,
        max_game_plies=50,
    )
    defaults.update(kwargs)
    if "model_path" not in defaults:
        defaults["model_path"] = get_test_model(game_id)
    return dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=seed, **defaults,
    )


def run_short_heuristic(game_id: str, seed: int = 42, temperature: float = 1.0) -> dict:
    return dinoboard_engine.run_heuristic_episode(
        game_id=game_id, seed=seed, temperature=temperature, max_game_plies=50,
    )
