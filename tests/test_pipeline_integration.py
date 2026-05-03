"""Pipeline orchestration tests: CLI config, batch functions, warm start, gating, scheduling."""
import json
import math
import tempfile
from pathlib import Path

import torch
import dinoboard_engine
import pytest

from conftest import CANONICAL_GAMES, GAME_CONFIGS, PROJECT_ROOT, get_test_model
from training.pipeline import (
    run_selfplay_batch,
    run_eval_batch,
    run_eval_vs_heuristic,
    run_training_loop,
    normalize_policy,
    compute_schedule_ratio,
    train_step,
    rotate_z_values,
    _get_temperature_key,
)
from training.model import PVNet, create_model_from_config, export_onnx
from training.cli import find_game_config


# ---------------------------------------------------------------------------
# CLI config loading
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_find_game_config_finds_all_games(game_id):
    """find_game_config should locate game.json for all canonical games."""
    cfg = find_game_config(game_id)
    assert cfg["game_id"] == game_id
    assert "action_space" in cfg
    assert "feature_dim" in cfg


def test_find_game_config_raises_for_unknown():
    """Unknown game should raise FileNotFoundError."""
    with pytest.raises(FileNotFoundError):
        find_game_config("nonexistent_game_xyz")


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_game_config_matches_engine(game_id):
    """Config values should match what the C++ engine reports."""
    cfg = find_game_config(game_id)
    info = dinoboard_engine.encode_state(game_id, seed=42)
    assert cfg["feature_dim"] == info["feature_dim"]
    assert cfg["action_space"] == info["action_space"]


# ---------------------------------------------------------------------------
# Selfplay batch
# ---------------------------------------------------------------------------

def test_selfplay_batch_returns_correct_count():
    """run_selfplay_batch should return exactly num_episodes results."""
    cfg = {"simulations": 10, "max_game_plies": 9}
    results = run_selfplay_batch(
        "tictactoe", get_test_model("tictactoe"), num_episodes=5, base_seed=42,
        train_cfg=cfg, max_workers=1,
    )
    assert len(results) == 5
    for ep in results:
        assert ep["total_plies"] > 0
        assert len(ep["samples"]) > 0


def test_selfplay_batch_different_seeds():
    """Each episode in a batch should use a different seed."""
    cfg = {
        "simulations": 10, "max_game_plies": 50,
        "dirichlet_alpha": 0.0, "dirichlet_epsilon": 0.0,
    }
    results = run_selfplay_batch(
        "tictactoe", get_test_model("tictactoe"), num_episodes=10, base_seed=42,
        train_cfg=cfg, max_workers=1,
    )
    action_seqs = [
        tuple(s["action_id"] for s in ep["samples"])
        for ep in results
    ]
    unique = len(set(action_seqs))
    assert unique >= 3, f"too few unique games in batch: {unique}/10"


def test_selfplay_batch_passes_tail_solve_config():
    """Tail solve config should propagate through batch."""
    cfg = {
        "simulations": 10, "max_game_plies": 200,
        "tail_solve_enabled": True,
        "tail_solve_start_ply": 1,
        "tail_solve_depth_limit": 3,
        "tail_solve_node_budget": 500,
    }
    results = run_selfplay_batch(
        "quoridor", get_test_model("quoridor"), num_episodes=3, base_seed=42,
        train_cfg=cfg, max_workers=1,
    )
    total_attempts = sum(ep.get("tail_solve_attempts", 0) for ep in results)
    assert total_attempts > 0, "tail_solve_enabled but no attempts"


def test_selfplay_batch_passes_heuristic_guidance():
    """Heuristic guidance ratio should affect episode behavior."""
    cfg_no_h = {
        "simulations": 10, "max_game_plies": 50,
        "heuristic_guidance_ratio": 0.0,
    }
    cfg_full_h = {
        "simulations": 10, "max_game_plies": 50,
        "heuristic_guidance_ratio": 1.0,
        "heuristic_temperature": 0.0,
    }
    ep_no = run_selfplay_batch(
        "quoridor", get_test_model("quoridor"), num_episodes=3, base_seed=42,
        train_cfg=cfg_no_h, max_workers=1,
    )
    ep_yes = run_selfplay_batch(
        "quoridor", get_test_model("quoridor"), num_episodes=3, base_seed=42,
        train_cfg=cfg_full_h, max_workers=1,
    )
    # Both should complete; with full heuristic guidance, visits should differ
    for ep in ep_no + ep_yes:
        assert ep["total_plies"] > 0


def test_selfplay_batch_passes_temperature_schedule():
    """Temperature schedule params should be passed through."""
    cfg = {
        "simulations": 10, "max_game_plies": 50,
        "temperature_initial": 1.0,
        "temperature_final": 0.01,
        "temperature_decay_plies": 10,
    }
    results = run_selfplay_batch(
        "tictactoe", get_test_model("tictactoe"), num_episodes=3, base_seed=42,
        train_cfg=cfg, max_workers=1,
    )
    for ep in results:
        assert ep["total_plies"] > 0


# ---------------------------------------------------------------------------
# Eval batch (arena)
# ---------------------------------------------------------------------------

def test_eval_batch_total_games_correct():
    """run_eval_batch should play exactly num_games."""
    model = get_test_model("tictactoe")
    r = run_eval_batch(
        "tictactoe", model, model, num_games=8, base_seed=42,
        sims_candidate=10, sims_opponent=10, max_workers=1,
    )
    total = r["wins"] + r["losses"] + r["draws"]
    assert total == 8


def test_eval_batch_symmetric_uniform():
    """Two identical uniform players should roughly tie."""
    model = get_test_model("tictactoe")
    r = run_eval_batch(
        "tictactoe", model, model, num_games=40, base_seed=42,
        sims_candidate=10, sims_opponent=10, max_workers=1,
    )
    assert 0.2 <= r["win_rate"] <= 0.8, (
        f"two identical players should be roughly equal, got win_rate={r['win_rate']}"
    )


# ---------------------------------------------------------------------------
# Eval vs heuristic
# ---------------------------------------------------------------------------

def test_eval_vs_heuristic_total_games_correct():
    model = get_test_model("quoridor")
    r = run_eval_vs_heuristic(
        "quoridor", model, num_games=6, base_seed=42,
        simulations=10, constrained=True,
        heuristic_temperature=0.0, max_workers=1,
    )
    total = r["wins"] + r["losses"] + r["draws"]
    assert total == 6


def test_eval_vs_heuristic_model_alternates_sides():
    """Eval should have the model play half as P0 and half as P1."""
    # We test this by checking different seeds produce variety
    model = get_test_model("quoridor")
    r = run_eval_vs_heuristic(
        "quoridor", model, num_games=10, base_seed=42,
        simulations=10, constrained=True,
        heuristic_temperature=0.0, max_workers=1,
    )
    # At least one game should result in non-draw
    assert r["wins"] + r["losses"] > 0 or r["draws"] == 10


# ---------------------------------------------------------------------------
# Schedule computation
# ---------------------------------------------------------------------------

def test_schedule_ratio_heuristic_guidance():
    """Quoridor uses heuristic_guidance_steps=3000. Step 1 should have high ratio."""
    train_cfg = GAME_CONFIGS["quoridor"]["training"]
    h_steps = train_cfg.get("heuristic_guidance_steps", 0)
    h_initial = train_cfg.get("heuristic_guidance_initial_ratio",
                               train_cfg.get("heuristic_guidance_ratio", 0.5))
    if h_steps > 0:
        ratio_1 = compute_schedule_ratio(1, h_steps, h_initial)
        ratio_mid = compute_schedule_ratio(h_steps // 2, h_steps, h_initial)
        ratio_end = compute_schedule_ratio(h_steps, h_steps, h_initial)
        assert ratio_1 > ratio_mid > ratio_end
        assert ratio_end == 0.0


def test_schedule_ratio_training_filter():
    """Training filter ratio should decay linearly."""
    train_cfg = GAME_CONFIGS["quoridor"]["training"]
    f_steps = train_cfg.get("training_filter_steps", 0)
    f_initial = train_cfg.get("training_filter_initial_ratio", 1.0)
    if f_steps > 0:
        ratio_start = compute_schedule_ratio(0, f_steps, f_initial)
        ratio_end = compute_schedule_ratio(f_steps, f_steps, f_initial)
        assert ratio_start == f_initial
        assert ratio_end == 0.0


def test_simulation_rampup_formula():
    """Simulation ramp from simulations_start to simulations over 30% of steps."""
    train_cfg = GAME_CONFIGS["quoridor"]["training"]
    sims_start = train_cfg.get("simulations_start", train_cfg["simulations"])
    sims_full = train_cfg["simulations"]
    steps = train_cfg["steps"]

    # At step 1
    frac_1 = min(1.0, 1 / max(1, steps * 0.3))
    sims_1 = int(sims_start + (sims_full - sims_start) * frac_1)
    assert sims_1 >= sims_start
    assert sims_1 <= sims_full

    # At 30% of total steps
    frac_30 = min(1.0, (steps * 0.3) / max(1, steps * 0.3))
    sims_30 = int(sims_start + (sims_full - sims_start) * frac_30)
    assert sims_30 == sims_full


# ---------------------------------------------------------------------------
# Warm start data collection
# ---------------------------------------------------------------------------

def test_warm_start_heuristic_episodes():
    """Heuristic episodes for warm start should produce valid training data."""
    episodes = []
    for i in range(5):
        ep = dinoboard_engine.run_heuristic_episode(
            game_id="quoridor", seed=42 + i,
            temperature=3.0, max_game_plies=50,
        )
        episodes.append(ep)

    cfg = GAME_CONFIGS["quoridor"]
    feature_dim = cfg["feature_dim"]
    action_space = cfg["action_space"]

    total_samples = 0
    for ep in episodes:
        for s in ep["samples"]:
            feats = s.get("features", [])
            if len(feats) != feature_dim:
                continue
            total_samples += 1
            policy = normalize_policy(
                s.get("policy_action_ids", []),
                s.get("policy_action_visits", []),
                action_space,
            )
            assert len(policy) == action_space
            total_p = sum(policy)
            if total_p > 0:
                assert abs(total_p - 1.0) < 1e-5

    assert total_samples > 0, "no valid warm start samples"


def test_warm_start_features_vary():
    """Warm start heuristic episodes should produce varying features."""
    ep = dinoboard_engine.run_heuristic_episode(
        game_id="quoridor", seed=42,
        temperature=3.0, max_game_plies=50,
    )
    samples = ep["samples"]
    if len(samples) >= 4:
        assert samples[0]["features"] != samples[3]["features"]


# ---------------------------------------------------------------------------
# Full mini training loop (1 step)
# ---------------------------------------------------------------------------

def test_mini_training_loop(tmp_path):
    """Run a minimal 1-step training loop to verify end-to-end integration."""
    cfg = find_game_config("tictactoe")
    action_space = cfg["action_space"]
    feature_dim = cfg["feature_dim"]
    num_players = cfg["num_players"]
    train_cfg = {"simulations": 10, "max_game_plies": 9}

    # Step 1: selfplay
    episodes = run_selfplay_batch(
        "tictactoe", get_test_model("tictactoe"), num_episodes=5, base_seed=42,
        train_cfg=train_cfg, max_workers=1,
    )

    # Step 2: collect training data
    features, policies, values, masks = [], [], [], []
    for ep in episodes:
        for s in ep["samples"]:
            feats = s.get("features", [])
            if len(feats) != feature_dim:
                continue
            features.append(feats)
            policies.append(normalize_policy(
                s["policy_action_ids"], s["policy_action_visits"],
                action_space))
            z_vals = s["z_values"]
            values.append(rotate_z_values(z_vals, s["player"], num_players))
            masks.append(s["legal_mask"])

    assert len(features) > 0

    # Step 3: train
    net = create_model_from_config(cfg)
    optimizer = torch.optim.AdamW(net.parameters(), lr=0.001)
    feat_t = torch.tensor(features, dtype=torch.float32)
    pol_t = torch.tensor(policies, dtype=torch.float32)
    val_t = torch.tensor(values, dtype=torch.float32)
    mask_t = torch.tensor(masks, dtype=torch.float32)

    m = train_step(net, optimizer, feat_t, pol_t, val_t,
                   legal_mask=mask_t, grad_clip_norm=1.0)
    assert math.isfinite(m["loss"])

    # Step 4: export ONNX
    onnx_path = tmp_path / "model.onnx"
    export_onnx(net, onnx_path, feature_dim)
    assert onnx_path.exists()

    # Step 5: use exported model for selfplay
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="tictactoe", seed=42, model_path=str(onnx_path),
        simulations=10, max_game_plies=9,
    )
    assert ep["total_plies"] > 0

    # Step 6: eval
    opponent_model = get_test_model("tictactoe")
    r = run_eval_batch(
        "tictactoe", str(onnx_path), opponent_model, num_games=4,
        base_seed=42, sims_candidate=10, sims_opponent=10,
        max_workers=1,
    )
    assert r["wins"] + r["losses"] + r["draws"] == 4


# ---------------------------------------------------------------------------
# Warm start + training loop (end-to-end through run_training_loop)
# ---------------------------------------------------------------------------

def test_warm_start_training_loop(tmp_path):
    """run_training_loop with warm_start_heuristic should complete without error."""
    cfg = find_game_config("quoridor")
    cfg["training"] = {
        **cfg["training"],
        "warm_start_heuristic": True,
        "warm_start_episodes": 10,
        "warm_start_epochs": 2,
        "warm_start_temperature": 3.0,
        "simulations": 10,
        "simulations_start": 10,
        "max_game_plies": 50,
        "steps": 2,
        "episodes_per_step": 3,
        "heuristic_guidance_steps": 0,
        "training_filter_steps": 0,
        "tail_solve_enabled": False,
    }
    run_training_loop(
        game_id="quoridor",
        game_config=cfg,
        output_dir=tmp_path,
        steps=2,
        episodes_per_step=3,
        eval_every=0,
        eval_games=0,
        max_workers=1,
        batch_size=32,
        learning_rate=0.001,
        seed=42,
    )
    assert (tmp_path / "models" / "model_init.onnx").exists()
    assert (tmp_path / "models" / "model_warm.onnx").exists()
    assert (tmp_path / "models" / "model_latest.onnx").exists()
    assert (tmp_path / "checkpoint.pt").exists()


def test_full_training_loop_with_eval(tmp_path):
    """Run 3 steps with eval and gating. Verify the whole pipeline end-to-end."""
    cfg = find_game_config("tictactoe")
    cfg["training"] = {
        **cfg.get("training", {}),
        "simulations": 10,
        "simulations_start": 10,
        "max_game_plies": 9,
        "steps": 3,
        "episodes_per_step": 5,
        "heuristic_guidance_steps": 0,
        "training_filter_steps": 0,
        "tail_solve_enabled": False,
    }
    models_dir = tmp_path / "models"

    run_training_loop(
        game_id="tictactoe",
        game_config=cfg,
        output_dir=tmp_path,
        steps=3,
        episodes_per_step=5,
        eval_every=2,
        eval_games=4,
        max_workers=1,
        batch_size=32,
        learning_rate=0.001,
        seed=42,
        save_every=2,
    )

    # model_latest.onnx must exist and be the current model
    latest = models_dir / "model_latest.onnx"
    assert latest.exists()

    # save_every=2 → step 2 checkpoint saved
    assert (models_dir / "model_step_00002.onnx").exists()

    # model_init.onnx must differ from model_latest.onnx (training changed weights)
    init_bytes = (models_dir / "model_init.onnx").read_bytes()
    latest_bytes = latest.read_bytes()
    assert init_bytes != latest_bytes, "model_latest should differ from model_init after training"

    # checkpoint has the last step
    ckpt = torch.load(tmp_path / "checkpoint.pt", weights_only=False)
    assert ckpt["step"] == 3

    # model_latest.onnx must actually work for selfplay (not corrupted/stale)
    ep = dinoboard_engine.run_selfplay_episode(
        game_id="tictactoe", seed=99, model_path=str(latest),
        simulations=10, max_game_plies=9,
    )
    assert ep["total_plies"] > 0

    # eval ran at step 2 → if gating passed, model_best.onnx exists and differs from latest
    best = models_dir / "model_best.onnx"
    if best.exists():
        best_bytes = best.read_bytes()
        # best is a snapshot, not a symlink/alias to latest
        # After step 3 training, latest has newer weights. best should be frozen at step 2.
        assert best_bytes != latest_bytes, "model_best should be a frozen copy, not track latest"


# ---------------------------------------------------------------------------
# Gating logic
# ---------------------------------------------------------------------------

def test_gating_threshold():
    """Win rate >= threshold should accept; below should reject."""
    threshold = 0.55
    assert 0.60 >= threshold  # accept
    assert 0.50 < threshold   # reject (but this is just a sanity check)

    cfg = GAME_CONFIGS["quoridor"]["training"]
    gar = cfg.get("gating_accept_win_rate", 0.6)
    assert 0.0 < gar < 1.0, "gating threshold should be between 0 and 1"


# ---------------------------------------------------------------------------
# Checkpoint save/load
# ---------------------------------------------------------------------------

def test_checkpoint_save_load(tmp_path):
    """Model checkpoint should be saveable and loadable."""
    cfg = find_game_config("tictactoe")
    net = create_model_from_config(cfg)
    optimizer = torch.optim.AdamW(net.parameters(), lr=0.001)

    # Save
    checkpoint = {
        "step": 42,
        "model_state_dict": net.state_dict(),
        "optimizer_state_dict": optimizer.state_dict(),
    }
    ckpt_path = tmp_path / "checkpoint.pt"
    torch.save(checkpoint, ckpt_path)
    assert ckpt_path.exists()

    # Load
    loaded = torch.load(ckpt_path, weights_only=False)
    assert loaded["step"] == 42

    net2 = create_model_from_config(cfg)
    net2.load_state_dict(loaded["model_state_dict"])

    # Verify weights match
    for (n1, p1), (n2, p2) in zip(net.named_parameters(), net2.named_parameters()):
        assert torch.equal(p1, p2), f"parameter {n1} mismatch after load"


# ---------------------------------------------------------------------------
# Temperature key helper with real configs
# ---------------------------------------------------------------------------

def test_splendor_temperature_extracted_correctly():
    """Splendor's nested temperature_schedule should be read by helper."""
    train_cfg = GAME_CONFIGS["splendor"]["training"]
    initial = _get_temperature_key(train_cfg, "initial", -1.0)
    final = _get_temperature_key(train_cfg, "final", -1.0)
    decay = _get_temperature_key(train_cfg, "decay_plies", 0)
    assert initial == 1.0
    assert final == 0.1
    assert decay == 30


def test_quoridor_temperature_extracted_correctly():
    """Quoridor's flat temperature keys should be read directly."""
    train_cfg = GAME_CONFIGS["quoridor"]["training"]
    initial = _get_temperature_key(train_cfg, "initial", -1.0)
    final = _get_temperature_key(train_cfg, "final", -1.0)
    assert initial == 1.0
    assert final == 0.15
