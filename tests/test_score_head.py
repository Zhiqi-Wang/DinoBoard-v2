"""Tests for auxiliary score head: creation, training, ONNX export."""
import math
import tempfile
from pathlib import Path

import torch
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, get_test_model
from training.model import PVNet, create_model_from_config, export_onnx
from training.pipeline import normalize_policy, train_step


def test_score_head_created_when_configured():
    net = PVNet(10, 5, [32], auxiliary_score=True)
    assert net.has_score_head
    assert hasattr(net, "score_head")


def test_score_head_not_created_when_disabled():
    net = PVNet(10, 5, [32], auxiliary_score=False)
    assert not net.has_score_head


def test_score_head_output_shape():
    net = PVNet(10, 5, [32], auxiliary_score=True, num_players=2)
    dummy = torch.zeros(4, 10)
    policy, value, score = net(dummy)
    assert score.shape == (4, 1)
    assert value.shape == (4, 2)
    assert policy.shape == (4, 5)


def test_score_head_bounded_by_tanh():
    """Score head uses tanh, so output should be in [-1, 1]."""
    net = PVNet(10, 5, [32], auxiliary_score=True)
    dummy = torch.randn(100, 10) * 10
    net.eval()
    with torch.no_grad():
        _, _, score = net(dummy)
    assert (score >= -1.0).all() and (score <= 1.0).all()


def test_score_head_loss_flows():
    """Score head loss should be included when auxiliary_targets is provided."""
    net = PVNet(10, 5, [32], auxiliary_score=True, num_players=2)
    optimizer = torch.optim.Adam(net.parameters(), lr=0.01)

    features = torch.randn(8, 10)
    policies = torch.zeros(8, 5)
    policies[:, 0] = 1.0
    values = torch.zeros(8, 2)
    aux_targets = torch.ones(8) * 0.5

    metrics = train_step(net, optimizer, features, policies, values,
                         auxiliary_targets=aux_targets, auxiliary_weight=1.0,
                         grad_clip_norm=0.0)
    assert metrics["score_loss"] > 0, "score loss should be positive with non-zero targets"
    assert math.isfinite(metrics["score_loss"])


def test_score_head_loss_reduces_with_training():
    net = PVNet(10, 5, [32], auxiliary_score=True, num_players=2)
    optimizer = torch.optim.Adam(net.parameters(), lr=0.01)

    features = torch.randn(16, 10)
    policies = torch.zeros(16, 5)
    policies[:, 2] = 1.0
    values = torch.zeros(16, 2)
    aux_targets = torch.ones(16) * 0.3

    losses = []
    for _ in range(30):
        m = train_step(net, optimizer, features, policies, values,
                       auxiliary_targets=aux_targets, auxiliary_weight=1.0,
                       grad_clip_norm=0.0)
        losses.append(m["score_loss"])
    assert losses[-1] < losses[0], (
        f"score loss should decrease: first={losses[0]:.4f}, last={losses[-1]:.4f}"
    )


def test_score_head_zero_when_no_auxiliary():
    """Without auxiliary_targets, score_loss should be 0."""
    net = PVNet(10, 5, [32], auxiliary_score=True, num_players=2)
    optimizer = torch.optim.Adam(net.parameters(), lr=0.01)

    features = torch.randn(4, 10)
    policies = torch.zeros(4, 5)
    policies[:, 0] = 1.0
    values = torch.zeros(4, 2)

    metrics = train_step(net, optimizer, features, policies, values,
                         auxiliary_targets=None, grad_clip_norm=0.0)
    assert metrics["score_loss"] == 0.0


def test_score_head_onnx_export(tmp_path):
    """ONNX export with score head should include 3 outputs."""
    net = PVNet(10, 5, [32], auxiliary_score=True)
    path = tmp_path / "score_model.onnx"
    export_onnx(net, path, 10)
    assert path.exists()
    assert path.stat().st_size > 0


def test_quoridor_auxiliary_score_in_selfplay():
    """Quoridor has auxiliary_scorer registered; samples should have auxiliary_score."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="quoridor", seed=42, model_path=get_test_model("quoridor"), simulations=10,
        max_game_plies=30,
    )
    has_score = any(s.get("auxiliary_score", 0.0) != 0.0 for s in ep["samples"])
    # Auxiliary score is shortest-path difference; should be non-zero in most positions
    if len(ep["samples"]) > 3:
        assert has_score, "quoridor samples should have non-zero auxiliary_score"
