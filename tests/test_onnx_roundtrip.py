"""ONNX export round-trip: PyTorch model → ONNX → C++ ONNX evaluator."""
import tempfile
from pathlib import Path

import torch
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, CANONICAL_GAMES, get_test_model
from training.model import PVNet, create_model_from_config, export_onnx


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_export_creates_file(game_id, tmp_path):
    cfg = GAME_CONFIGS[game_id]
    net = create_model_from_config(cfg)
    onnx_path = tmp_path / f"{game_id}.onnx"
    result = export_onnx(net, onnx_path, cfg["feature_dim"])
    assert Path(result).exists()
    assert Path(result).stat().st_size > 0


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_export_with_score_head(game_id, tmp_path):
    cfg = GAME_CONFIGS[game_id]
    net = PVNet(cfg["feature_dim"], cfg["action_space"], [64, 64], auxiliary_score=True)
    onnx_path = tmp_path / f"{game_id}_score.onnx"
    export_onnx(net, onnx_path, cfg["feature_dim"])
    assert onnx_path.exists()


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_onnx_model_usable_in_selfplay(game_id, tmp_path):
    """Exported ONNX model should work as the evaluator in a selfplay episode."""
    cfg = GAME_CONFIGS[game_id]
    net = create_model_from_config(cfg)
    onnx_path = tmp_path / f"{game_id}.onnx"
    export_onnx(net, onnx_path, cfg["feature_dim"])

    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=str(onnx_path),
        simulations=10, max_game_plies=30,
    )
    assert len(ep["samples"]) > 0
    assert ep["total_plies"] > 0


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_onnx_model_usable_in_game_session(game_id, tmp_path):
    """Exported model should work for GameSession AI actions."""
    cfg = GAME_CONFIGS[game_id]
    net = create_model_from_config(cfg)
    onnx_path = tmp_path / f"{game_id}.onnx"
    export_onnx(net, onnx_path, cfg["feature_dim"])

    gs = dinoboard_engine.GameSession(game_id, seed=42, model_path=str(onnx_path))
    result = gs.get_ai_action(simulations=10, temperature=0.0)
    legal = gs.get_all_legal_actions()
    assert result["action"] in legal


def test_onnx_selfplay_differs_from_uniform():
    """ONNX model should produce different visit patterns than uniform evaluator."""
    cfg = GAME_CONFIGS["tictactoe"]
    net = create_model_from_config(cfg)
    # Train briefly to get non-uniform outputs
    optimizer = torch.optim.Adam(net.parameters(), lr=0.1)
    dummy_x = torch.randn(16, cfg["feature_dim"])
    dummy_policy = torch.zeros(16, cfg["action_space"])
    dummy_policy[:, 4] = 1.0  # bias toward center
    for _ in range(50):
        logits, val = net(dummy_x)
        loss = -(dummy_policy * torch.nn.functional.log_softmax(logits, dim=-1)).sum(-1).mean()
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

    with tempfile.TemporaryDirectory() as tmp:
        onnx_path = Path(tmp) / "biased.onnx"
        export_onnx(net, onnx_path, cfg["feature_dim"])

        ep_model = dinoboard_engine.run_selfplay_episode(
            game_id="tictactoe", seed=42, model_path=str(onnx_path),
            simulations=50, max_game_plies=9,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        ep_random = dinoboard_engine.run_selfplay_episode(
            game_id="tictactoe", seed=42, model_path=get_test_model("tictactoe"),
            simulations=50, max_game_plies=9,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )

    if ep_model["samples"] and ep_random["samples"]:
        model_visits = ep_model["samples"][0]["policy_action_visits"]
        random_visits = ep_random["samples"][0]["policy_action_visits"]
        assert model_visits != random_visits, (
            "biased ONNX model should produce different visits than random model"
        )
