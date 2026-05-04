"""Regression: deployed ONNX models in games/<name>/model/ must match the
CURRENT encoder's feature_dim.

Motivation: silent staleness. If an encoder gains a feature dim (e.g. new
`first_player` indicator in TicTacToe) but the deployed model on disk was
trained under the old dim, ONNX evaluator throws at runtime — pipelines
show "thinking" forever, users see no AI move, no clear error in UI.

This test catches that at CI time by loading each deployed model through
onnxruntime and asserting the input shape matches `game_metadata.feature_dim`.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

import dinoboard_engine as engine


DEPLOYED_MODEL_GAMES = ["tictactoe", "quoridor", "splendor", "azul", "loveletter", "coup"]


def _deployed_model_path(game_id: str) -> Path | None:
    """Match find_model_path in platform/game_service/sessions.py. Returns
    Path if deployed model file exists, None otherwise."""
    base = game_id
    import re
    base = re.sub(r"_\d+p$", "", base)
    model_dir = PROJECT_ROOT / "games" / base / "model"
    variant_name = game_id if game_id != base else f"{base}_2p"
    candidate = model_dir / f"{variant_name}.onnx"
    if candidate.exists():
        return candidate
    alt = model_dir / f"{game_id}.onnx"
    if alt.exists():
        return alt
    return None


@pytest.mark.parametrize("game_id", DEPLOYED_MODEL_GAMES)
def test_deployed_model_feature_dim_matches_encoder(game_id):
    """Deployed ONNX input dim must match current encoder feature_dim."""
    onnx = pytest.importorskip("onnxruntime")
    model_path = _deployed_model_path(game_id)
    if model_path is None:
        pytest.skip(f"no deployed model for {game_id}")

    meta = engine.game_metadata(game_id)
    expected_dim = meta["feature_dim"]

    sess = onnx.InferenceSession(
        str(model_path), providers=["CPUExecutionProvider"])
    inputs = sess.get_inputs()
    assert len(inputs) == 1, (
        f"{game_id}: deployed model has {len(inputs)} inputs, expected 1")
    shape = inputs[0].shape  # typically [batch, feature_dim]
    assert len(shape) == 2, (
        f"{game_id}: deployed model input shape {shape} is not 2-D")
    actual_dim = shape[1]
    assert actual_dim == expected_dim, (
        f"{game_id}: deployed model at {model_path} has feature_dim={actual_dim} "
        f"but current encoder produces {expected_dim}. Regenerate the model.")
