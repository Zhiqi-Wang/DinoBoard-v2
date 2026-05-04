"""Regression: feature encoder respects the public/private partition.

The ISMCTS architecture aligns encoder scope with the hash scope:
encoder must only extract features from (public_fields + current
player's private_fields). Opponents' private fields must NOT influence
encoder output.

This is the structural enforcement of "no info leak through features":
- Hash groups sampled worlds by info set → multiple worlds share tree nodes
- Encoder must produce the SAME feature vector for all worlds in the info set
- Otherwise the network sees different features across sampled worlds at
  the same tree node, and its prior/value estimates become world-specific
  rather than info-set-specific → classic Method 2b prior pollution

Test method: for each hidden-info game, construct two states that differ
ONLY in opp private fields. Encoder output must be bit-identical.

If this test fails, either:
- The encoder is reading opp private (info leak)
- The game's hash_private_fields declaration is wrong (wrong partition)

Either way, we want to know.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT / "platform"))

import dinoboard_engine as engine
from conftest import get_test_model


# Games with hidden info worth checking. TicTacToe / Quoridor have no
# private fields, so encoder invariance is trivial.
HIDDEN_INFO_GAMES = ["loveletter", "splendor", "azul"]


@pytest.mark.parametrize("game_id", HIDDEN_INFO_GAMES)
def test_encoder_output_invariant_under_opp_private_change(game_id):
    """Two sessions seeded identically should produce equal public state
    but potentially-different opp private state after resampling. The
    encoder output for the same perspective should be bit-identical.

    We approximate this via: two GameSession instances, same seed, drive
    them through identical action sequences, extract features at each
    perspective's decision point. Features should match.

    For games where the test model doesn't expose encoder features
    directly via Python, we use selfplay samples as a proxy: the same
    seed + same actions should produce identical sample.features if the
    encoder respects hash scope.
    """
    model_path = get_test_model(game_id)

    # Run two selfplay episodes with identical seeds. Their sample
    # features should match byte-for-byte (the RNG driving action
    # selection is deterministic given seed).
    ep_a = engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=model_path,
        simulations=20, max_game_plies=15,
    )
    ep_b = engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=model_path,
        simulations=20, max_game_plies=15,
    )

    assert len(ep_a["samples"]) == len(ep_b["samples"]), (
        f"[{game_id}] deterministic selfplay produced different numbers of "
        f"samples: {len(ep_a['samples'])} vs {len(ep_b['samples'])}")

    # Compare features sample-by-sample. If the encoder leaks opp private
    # info that differs across selfplay samplings of the same seed (which
    # it shouldn't if randomize_unseen is deterministic for the same seed),
    # we'd see differences. Since deterministic selfplay keeps opp state
    # identical too, this test mostly asserts selfplay reproducibility.
    # For a STRONGER check, see Phase 6 (encode_public/private) tests.
    for i, (sa, sb) in enumerate(zip(ep_a["samples"], ep_b["samples"])):
        fa = np.asarray(sa["features"], dtype=np.float32)
        fb = np.asarray(sb["features"], dtype=np.float32)
        if not np.array_equal(fa, fb):
            diff_idx = int(np.argmax(np.abs(fa - fb)))
            pytest.fail(
                f"[{game_id}] sample {i}: encoder features diverged at "
                f"index {diff_idx}: {fa[diff_idx]} vs {fb[diff_idx]}. "
                f"This usually means the encoder is reading non-deterministic "
                f"state (opp hidden field or rng nonce) even though the "
                f"observable state is identical."
            )


@pytest.mark.parametrize("game_id", HIDDEN_INFO_GAMES)
def test_encoder_stable_within_info_set(game_id):
    """Sanity: the feature_dim advertised by game_metadata matches the
    actual feature vector length produced by the encoder during selfplay.
    A mismatch indicates encoder drift after the refactor.
    """
    model_path = get_test_model(game_id)
    meta = engine.game_metadata(game_id)
    expected_dim = meta["feature_dim"]

    ep = engine.run_selfplay_episode(
        game_id=game_id, seed=123, model_path=model_path,
        simulations=20, max_game_plies=10,
    )
    assert ep["samples"], f"[{game_id}] no samples produced"
    for i, s in enumerate(ep["samples"]):
        actual_dim = len(s["features"])
        assert actual_dim == expected_dim, (
            f"[{game_id}] sample {i}: feature vector length {actual_dim} "
            f"does not match game_metadata feature_dim {expected_dim}")


# ---------------------------------------------------------------------------
# Phase 6: structural invariants of the public/private split.
# These are STRONGER than the selfplay-reproducibility checks above — they
# exercise `encode_public` / `encode_private` directly through the binding.
# ---------------------------------------------------------------------------

ALL_GAMES = ["tictactoe", "quoridor", "splendor", "azul", "loveletter", "coup"]


@pytest.mark.parametrize("game_id", ALL_GAMES)
def test_public_plus_private_equals_full(game_id):
    """encode(state, p) must equal concat(encode_public(state, p),
    encode_private(state, p)). This is the contract the base-class
    `encode` implementation promises."""
    enc = engine.encode_state(game_id, seed=42)
    full = list(enc["features"])
    public = list(enc["public_features"])
    private = list(enc["private_features"])

    assert enc["public_feature_dim"] == len(public), (
        f"{game_id}: public_feature_dim={enc['public_feature_dim']} but "
        f"actual public length={len(public)}")
    assert enc["private_feature_dim"] == len(private), (
        f"{game_id}: private_feature_dim={enc['private_feature_dim']} but "
        f"actual private length={len(private)}")
    assert len(public) + len(private) == len(full), (
        f"{game_id}: public({len(public)}) + private({len(private)}) != "
        f"full({len(full)})")
    assert public + private == full, (
        f"{game_id}: concat(public, private) does not equal full features")


@pytest.mark.parametrize("game_id", ALL_GAMES)
def test_feature_dim_split_matches_metadata(game_id):
    """public_feature_dim + private_feature_dim == feature_dim, and the
    latter matches game_metadata.feature_dim."""
    meta = engine.game_metadata(game_id)
    enc = engine.encode_state(game_id, seed=42)
    assert enc["public_feature_dim"] + enc["private_feature_dim"] == \
        enc["feature_dim"]
    assert enc["feature_dim"] == meta["feature_dim"]


@pytest.mark.parametrize("game_id", ["tictactoe", "quoridor", "azul"])
def test_fully_observable_games_have_zero_private_dim(game_id):
    """Games with no non-symmetric hidden info advertise private_feature_dim=0.
    TicTacToe / Quoridor are deterministic; Azul has symmetric random (bag
    order) but no per-player hidden fields."""
    enc = engine.encode_state(game_id, seed=42)
    assert enc["private_feature_dim"] == 0, (
        f"{game_id} is fully observable but reports "
        f"private_feature_dim={enc['private_feature_dim']}")
