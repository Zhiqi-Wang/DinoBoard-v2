"""Self-play training pipeline: selfplay -> collect samples -> train -> export -> repeat."""
from __future__ import annotations

import json
import logging
import shutil
import time
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any

import torch

from .model import PVNet, create_model_from_config, export_onnx

logger = logging.getLogger(__name__)


def _get_temperature_key(train_cfg: dict, key: str, default):
    """Read temperature param from flat key or nested temperature_schedule."""
    flat = train_cfg.get(f"temperature_{key}")
    if flat is not None:
        return flat
    sched = train_cfg.get("temperature_schedule")
    if isinstance(sched, dict):
        return sched.get(key, default)
    return default


def _worker_selfplay(args: tuple) -> dict[str, Any]:
    """Run a single selfplay episode in a worker process."""
    import dinoboard_engine
    game_id, seed, model_path, cfg = args
    return dinoboard_engine.run_selfplay_episode(
        game_id=game_id,
        seed=seed,
        model_path=model_path,
        simulations=cfg["simulations"],
        c_puct=cfg.get("c_puct", 1.4),
        temperature=cfg.get("temperature", 1.0),
        dirichlet_alpha=cfg.get("dirichlet_alpha", 0.3),
        dirichlet_epsilon=cfg.get("dirichlet_epsilon", 0.25),
        dirichlet_on_first_n_plies=cfg.get("dirichlet_on_first_n_plies", 30),
        max_game_plies=cfg.get("max_game_plies", 500),
        tail_solve_enabled=cfg.get("tail_solve_enabled", False),
        tail_solve_start_ply=cfg.get("tail_solve_start_ply", 40),
        tail_solve_depth_limit=cfg.get("tail_solve_depth_limit", 5),
        tail_solve_node_budget=cfg.get("tail_solve_node_budget", 10000000),
        tail_solve_margin_weight=cfg.get("tail_solve_margin_weight", 0.0),
        temperature_initial=cfg.get("temperature_initial", -1.0),
        temperature_final=cfg.get("temperature_final", -1.0),
        temperature_decay_plies=cfg.get("temperature_decay_plies", 0),
        heuristic_guidance_ratio=cfg.get("heuristic_guidance_ratio", 0.0),
        heuristic_temperature=cfg.get("heuristic_temperature", 0.0),
        training_filter_ratio=cfg.get("training_filter_ratio", 1.0),
        nopeek_enabled=cfg.get("nopeek_enabled", True),
    )


def _worker_arena(args: tuple) -> dict[str, Any]:
    """Run a single arena match in a worker process."""
    import dinoboard_engine
    game_id, seed, model_paths, sims_list, max_plies, temperature = args
    return dinoboard_engine.run_arena_match(
        game_id=game_id, seed=seed,
        model_paths=model_paths, simulations_list=sims_list,
        temperature=temperature,
        max_game_plies=max_plies,
    )


def _worker_eval_vs_heuristic(args: tuple) -> dict[str, Any]:
    """Run a single eval game vs heuristic in a worker process."""
    import dinoboard_engine
    game_id, seed, model_path, simulations, model_is_player, constrained, h_temp = args
    return dinoboard_engine.run_constrained_eval_vs_heuristic(
        game_id=game_id,
        seed=seed,
        model_path=model_path,
        simulations=simulations,
        model_is_player=model_is_player,
        constrained=constrained,
        heuristic_temperature=h_temp,
    )


def _worker_warm_start(args: tuple) -> dict[str, Any]:
    """Run a single heuristic episode for warm-start data."""
    import dinoboard_engine
    game_id, seed, temperature, max_plies = args
    return dinoboard_engine.run_heuristic_episode(
        game_id=game_id,
        seed=seed,
        temperature=temperature,
        max_game_plies=max_plies,
    )


def normalize_policy(action_ids: list[int], visits: list[int], action_space: int) -> list[float]:
    total = sum(max(0, v) for v in visits)
    policy = [0.0] * action_space
    if total <= 0:
        return policy
    for aid, v in zip(action_ids, visits):
        if 0 <= aid < action_space:
            policy[aid] = max(0, v) / total
    return policy


def train_step(
    net: PVNet,
    optimizer: torch.optim.Optimizer,
    features: torch.Tensor,
    policy_targets: torch.Tensor,
    value_targets: torch.Tensor,
    legal_mask: torch.Tensor | None = None,
    auxiliary_targets: torch.Tensor | None = None,
    auxiliary_weight: float = 0.5,
    grad_clip_norm: float = 1.0,
) -> dict[str, float]:
    net.train()
    outputs = net(features)
    if net.has_score_head:
        policy_logits, value_pred, score_pred = outputs
    else:
        policy_logits, value_pred = outputs

    if legal_mask is not None:
        policy_logits = policy_logits.masked_fill(legal_mask == 0, -1e9)

    value_loss = torch.nn.functional.mse_loss(value_pred, value_targets)
    policy_loss = -(policy_targets * torch.nn.functional.log_softmax(policy_logits, dim=-1)).sum(dim=-1).mean()
    loss = policy_loss + value_loss

    if net.has_score_head and auxiliary_targets is not None:
        score_loss = torch.nn.functional.mse_loss(score_pred.squeeze(-1), auxiliary_targets)
        loss = loss + auxiliary_weight * score_loss
    else:
        score_loss = torch.tensor(0.0)

    optimizer.zero_grad()
    loss.backward()
    if grad_clip_norm > 0:
        torch.nn.utils.clip_grad_norm_(net.parameters(), grad_clip_norm)
    optimizer.step()
    return {
        "loss": loss.item(),
        "policy_loss": policy_loss.item(),
        "value_loss": value_loss.item(),
        "score_loss": score_loss.item(),
    }


def run_selfplay_batch(
    game_id: str,
    model_path: str,
    num_episodes: int,
    base_seed: int,
    train_cfg: dict,
    max_workers: int,
) -> list[dict[str, Any]]:
    tasks = [
        (game_id, base_seed + i, model_path, train_cfg)
        for i in range(num_episodes)
    ]
    results = []
    with ProcessPoolExecutor(max_workers=max_workers) as pool:
        for r in pool.map(_worker_selfplay, tasks):
            results.append(r)
    return results


def run_eval_vs_heuristic(
    game_id: str,
    model_path: str,
    num_games: int,
    base_seed: int,
    simulations: int,
    constrained: bool,
    heuristic_temperature: float,
    max_workers: int,
) -> dict[str, Any]:
    tasks = []
    for i in range(num_games):
        model_side = i % 2
        tasks.append((
            game_id, base_seed + i, model_path, simulations,
            model_side, constrained, heuristic_temperature,
        ))

    wins = losses = draws = 0
    with ProcessPoolExecutor(max_workers=max_workers) as pool:
        for idx, r in enumerate(pool.map(_worker_eval_vs_heuristic, tasks)):
            model_side = tasks[idx][4]
            w = r["winner"]
            if r["draw"] or w < 0:
                draws += 1
            elif w == model_side:
                wins += 1
            else:
                losses += 1

    total = wins + losses + draws
    win_rate = wins / max(1, total)
    return {"wins": wins, "losses": losses, "draws": draws, "win_rate": win_rate}


def run_eval_batch(
    game_id: str,
    candidate_path: str,
    opponent_path: str,
    num_games: int,
    base_seed: int,
    sims_candidate: int,
    sims_opponent: int,
    max_workers: int,
    max_game_plies: int = 500,
    temperature: float = 0.0,
) -> dict[str, Any]:
    import dinoboard_engine
    meta = dinoboard_engine.game_metadata(game_id)
    num_players = meta["num_players"]

    tasks = []
    candidate_seats = []
    for i in range(num_games):
        seat = i % num_players
        paths = [opponent_path] * num_players
        sims = [sims_opponent] * num_players
        paths[seat] = candidate_path
        sims[seat] = sims_candidate
        tasks.append((game_id, base_seed + i, paths, sims, max_game_plies, temperature))
        candidate_seats.append(seat)

    wins = losses = draws = 0
    with ProcessPoolExecutor(max_workers=max_workers) as pool:
        for idx, r in enumerate(pool.map(_worker_arena, tasks)):
            w = r["winner"]
            if r["draw"] or w < 0:
                draws += 1
            elif w == candidate_seats[idx]:
                wins += 1
            else:
                losses += 1

    total = wins + losses + draws
    win_rate = wins / max(1, total)
    return {"wins": wins, "losses": losses, "draws": draws, "win_rate": win_rate}


def rotate_z_values(z_values: list[float], player: int, num_players: int = 0) -> list[float]:
    if not z_values:
        raise ValueError("z_values is empty — C++ must always populate z_values for every sample")
    n = len(z_values)
    return [z_values[(player + i) % n] for i in range(n)]


def compute_schedule_ratio(step: int, total_steps: int, initial_ratio: float) -> float:
    if step >= total_steps:
        return 0.0
    return initial_ratio * (1.0 - step / total_steps)


def run_training_loop(
    game_id: str,
    game_config: dict,
    output_dir: Path,
    steps: int = 1000,
    episodes_per_step: int = 200,
    eval_every: int = 50,
    eval_games: int = 40,
    eval_benchmarks: list[str] | None = None,
    max_workers: int = 4,
    batch_size: int = 512,
    learning_rate: float = 0.001,
    seed: int = 20260323,
    save_every: int = 0,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    models_dir = output_dir / "models"
    models_dir.mkdir(exist_ok=True)

    train_cfg = game_config["training"]
    action_space = game_config["action_space"]
    feature_dim = game_config["feature_dim"]
    if "num_players" not in game_config:
        raise KeyError(
            "game_config missing 'num_players'. Inject from engine.game_metadata(game_id) "
            "before calling run_training_loop — JSON 'players.max' is not authoritative."
        )
    num_players = game_config["num_players"]
    auxiliary_score = train_cfg.get("auxiliary_score", False)
    auxiliary_score_weight = train_cfg.get("auxiliary_score_weight", 0.5)

    if save_every <= 0:
        save_every = eval_every if eval_every > 0 else 50

    net = create_model_from_config(game_config)
    torch.manual_seed(seed)
    weight_decay = train_cfg.get("weight_decay", 1e-4)
    optimizer = torch.optim.AdamW(net.parameters(), lr=learning_rate, weight_decay=weight_decay)

    initial_onnx = models_dir / "model_init.onnx"
    export_onnx(net, initial_onnx, feature_dim)

    # Warm start: pre-train on heuristic episodes
    warm_start_episodes = train_cfg.get("warm_start_episodes", 0)
    warm_start_epochs = train_cfg.get("warm_start_epochs", 5)
    warm_start_heuristic = train_cfg.get("warm_start_heuristic", False)
    warm_start_temperature = train_cfg.get("warm_start_temperature", 3.0)

    if warm_start_heuristic and warm_start_episodes > 0:
        logger.info(f"Warm start: collecting {warm_start_episodes} heuristic episodes (temp={warm_start_temperature})")
        max_plies = train_cfg.get("max_game_plies", 200)
        warm_tasks = [
            (game_id, seed + i, warm_start_temperature, max_plies)
            for i in range(warm_start_episodes)
        ]
        warm_episodes = []
        with ProcessPoolExecutor(max_workers=max_workers) as pool:
            for r in pool.map(_worker_warm_start, warm_tasks):
                warm_episodes.append(r)

        warm_features, warm_policies, warm_values, warm_masks, warm_aux = [], [], [], [], []
        for ep in warm_episodes:
            for sample in ep["samples"]:
                feats = sample["features"]
                if len(feats) != feature_dim:
                    raise ValueError(
                        f"warm start: feature dim mismatch: got {len(feats)}, expected {feature_dim}")
                warm_features.append(feats)
                policy = normalize_policy(
                    sample["policy_action_ids"],
                    sample["policy_action_visits"],
                    action_space,
                )
                warm_policies.append(policy)
                z_vals = sample["z_values"]
                player = sample["player"]
                warm_values.append(rotate_z_values(z_vals, player, num_players))
                warm_masks.append(sample["legal_mask"])
                warm_aux.append(float(sample["auxiliary_score"]))

        if warm_features:
            feat_t = torch.tensor(warm_features, dtype=torch.float32)
            pol_t = torch.tensor(warm_policies, dtype=torch.float32)
            val_t = torch.tensor(warm_values, dtype=torch.float32)
            mask_t = torch.tensor(warm_masks, dtype=torch.float32)
            aux_t = torch.tensor(warm_aux, dtype=torch.float32) if auxiliary_score else None
            n = feat_t.size(0)
            grad_clip = train_cfg.get("grad_clip_norm", 1.0)
            logger.info(f"Warm start: training on {n} samples for {warm_start_epochs} epochs")
            for epoch in range(warm_start_epochs):
                perm = torch.randperm(n)
                total_loss = 0.0
                num_batches = max(1, n // batch_size)
                for b in range(num_batches):
                    idx = perm[b * batch_size: (b + 1) * batch_size]
                    metrics = train_step(
                        net, optimizer, feat_t[idx], pol_t[idx], val_t[idx],
                        legal_mask=mask_t[idx],
                        auxiliary_targets=aux_t[idx] if aux_t is not None else None,
                        auxiliary_weight=auxiliary_score_weight,
                        grad_clip_norm=grad_clip,
                    )
                    total_loss += metrics["loss"]
                avg = total_loss / max(1, num_batches)
                logger.info(f"  warm epoch {epoch+1}/{warm_start_epochs}: loss={avg:.4f}")

            warm_onnx = models_dir / "model_warm.onnx"
            export_onnx(net, warm_onnx, feature_dim)
            logger.info(f"Warm start model exported: {warm_onnx}")

    current_model_path = str(models_dir / "model_warm.onnx") if (models_dir / "model_warm.onnx").exists() else str(initial_onnx)
    best_model_path = current_model_path

    # Scheduling parameters
    heuristic_guidance_steps = train_cfg.get("heuristic_guidance_steps", 0)
    heuristic_guidance_initial = train_cfg.get("heuristic_guidance_ratio",
                                                train_cfg.get("heuristic_guidance_initial_ratio", 0.5))
    training_filter_steps = train_cfg.get("training_filter_steps", 0)
    training_filter_initial = train_cfg.get("training_filter_initial_ratio", 0.5)
    peek_steps = train_cfg.get("peek_steps", 0)

    simulations_start = train_cfg.get("simulations_start", train_cfg.get("simulations", 200))
    simulations_full = train_cfg.get("simulations", 200)

    replay_buffer_size = episodes_per_step * 50 * 20
    replay_buffer: deque[tuple[list, list, list, list, float]] = deque(maxlen=replay_buffer_size)

    # Seed replay buffer with warm start data so step 1 trains on a rich buffer
    if warm_start_heuristic and warm_start_episodes > 0 and warm_features:
        for i in range(len(warm_features)):
            replay_buffer.append((
                warm_features[i], warm_policies[i], warm_values[i],
                warm_masks[i], warm_aux[i],
            ))
        logger.info(f"Warm start: seeded replay buffer with {len(replay_buffer)} samples")

    logger.info(f"Starting training: game={game_id}, steps={steps}, episodes/step={episodes_per_step}")
    logger.info(f"  heuristic_guidance_steps={heuristic_guidance_steps}, initial_ratio={heuristic_guidance_initial}")
    logger.info(f"  training_filter_steps={training_filter_steps}, initial_ratio={training_filter_initial}")
    logger.info(f"  simulations: start={simulations_start}, full={simulations_full}")
    logger.info(f"  peek_steps={peek_steps}")
    logger.info(f"  tail_solve: enabled={train_cfg.get('tail_solve_enabled', False)}")
    logger.info(f"  replay_buffer: maxlen={replay_buffer_size}")

    for step in range(1, steps + 1):
        t0 = time.perf_counter()

        # Compute scheduled ratios
        heuristic_ratio = compute_schedule_ratio(step, heuristic_guidance_steps, heuristic_guidance_initial)
        filter_ratio = compute_schedule_ratio(step, training_filter_steps, training_filter_initial)

        # Ramp simulations
        sim_frac = min(1.0, step / max(1, steps * 0.3))
        current_sims = int(simulations_start + (simulations_full - simulations_start) * sim_frac)

        selfplay_cfg = {
            "simulations": current_sims,
            "c_puct": train_cfg.get("c_puct", 1.4),
            "temperature": train_cfg.get("temperature", 1.0),
            "dirichlet_alpha": train_cfg.get("dirichlet_alpha", 0.3),
            "dirichlet_epsilon": train_cfg.get("dirichlet_epsilon", 0.25),
            "dirichlet_on_first_n_plies": train_cfg.get("dirichlet_on_first_n_plies", 30),
            "max_game_plies": train_cfg.get("max_game_plies", 500),
            "tail_solve_enabled": train_cfg.get("tail_solve_enabled", False),
            "tail_solve_start_ply": train_cfg.get("tail_solve_start_ply", 40),
            "tail_solve_depth_limit": train_cfg.get("tail_solve_depth_limit", 5),
            "tail_solve_node_budget": train_cfg.get("tail_solve_node_budget", 10000000),
            "tail_solve_margin_weight": train_cfg.get("tail_solve_margin_weight", 0.0),
            "temperature_initial": _get_temperature_key(train_cfg, "initial", -1.0),
            "temperature_final": _get_temperature_key(train_cfg, "final", -1.0),
            "temperature_decay_plies": _get_temperature_key(train_cfg, "decay_plies", 0),
            "heuristic_guidance_ratio": heuristic_ratio,
            "heuristic_temperature": train_cfg.get("heuristic_temperature", 0.0),
            "training_filter_ratio": filter_ratio,
            # peek_steps=N means "first N steps use peek". When step < N the
            # searcher disables root sampling (nopeek_enabled=False) and runs
            # on truth; step N onwards switches to ISMCTS (nopeek_enabled=True).
            # Off-by-one caveat: `step > peek_steps` would wrongly include
            # step==peek_steps in the peek window — use `>=` to match the
            # "first N steps" semantics (peek_steps=0 means no peek at all).
            "nopeek_enabled": step >= peek_steps,
        }

        episodes = run_selfplay_batch(
            game_id, current_model_path, episodes_per_step,
            seed + step * 10000, selfplay_cfg, max_workers)

        for ep in episodes:
            for sample in ep["samples"]:
                feats = sample["features"]
                if len(feats) != feature_dim:
                    raise ValueError(
                        f"step {step}: feature dim mismatch: got {len(feats)}, expected {feature_dim}")
                policy = normalize_policy(
                    sample["policy_action_ids"],
                    sample["policy_action_visits"],
                    action_space,
                )
                z_vals = sample["z_values"]
                player = sample["player"]
                z_rotated = rotate_z_values(z_vals, player, num_players)
                mask = sample["legal_mask"]
                aux = float(sample["auxiliary_score"])
                replay_buffer.append((feats, policy, z_rotated, mask, aux))

        winners = [ep["winner"] for ep in episodes]
        per_player_wins = [sum(1 for w in winners if w == p) for p in range(num_players)]
        draws_count = sum(1 for w in winners if w < 0)

        step_samples = sum(len(ep["samples"]) for ep in episodes)

        ts_attempts = sum(ep["tail_solve_attempts"] for ep in episodes)
        ts_completed = sum(ep["tail_solve_completed"] for ep in episodes)
        ts_successes = sum(ep["tail_solve_successes"] for ep in episodes)

        if not replay_buffer:
            logger.warning(f"Step {step}: no training samples, skipping")
            continue

        buf = list(replay_buffer)
        all_features = [s[0] for s in buf]
        all_policies = [s[1] for s in buf]
        all_values = [s[2] for s in buf]
        all_masks = [s[3] for s in buf]
        all_aux = [s[4] for s in buf]

        feat_tensor = torch.tensor(all_features, dtype=torch.float32)
        policy_tensor = torch.tensor(all_policies, dtype=torch.float32)
        value_tensor = torch.tensor(all_values, dtype=torch.float32)
        mask_tensor = torch.tensor(all_masks, dtype=torch.float32)
        aux_tensor = torch.tensor(all_aux, dtype=torch.float32) if auxiliary_score else None

        n = feat_tensor.size(0)
        grad_clip = train_cfg.get("grad_clip_norm", 1.0)
        train_batches = train_cfg.get("train_batches_per_step", 3)
        total_loss = 0.0
        for b in range(train_batches):
            idx = torch.randint(n, (min(batch_size, n),))
            metrics = train_step(
                net, optimizer,
                feat_tensor[idx], policy_tensor[idx], value_tensor[idx],
                legal_mask=mask_tensor[idx],
                auxiliary_targets=aux_tensor[idx] if aux_tensor is not None else None,
                auxiliary_weight=auxiliary_score_weight,
                grad_clip_norm=grad_clip,
            )
            total_loss += metrics["loss"]
        avg_loss = total_loss / max(1, train_batches)

        latest_onnx = models_dir / "model_latest.onnx"
        export_onnx(net, latest_onnx, feature_dim)
        current_model_path = str(latest_onnx)

        if step % save_every == 0:
            step_onnx = models_dir / f"model_step_{step:05d}.onnx"
            export_onnx(net, step_onnx, feature_dim)

        elapsed = time.perf_counter() - t0
        log_parts = [
            f"Step {step}/{steps}: loss={avg_loss:.4f}",
            f"episodes={len(episodes)}, samples={n}",
            ", ".join(f"p{p}={per_player_wins[p]}" for p in range(num_players)) + f", draw={draws_count}",
            f"sims={current_sims}",
        ]
        if heuristic_ratio > 0:
            log_parts.append(f"h_ratio={heuristic_ratio:.2f}")
        if filter_ratio > 0:
            log_parts.append(f"f_ratio={filter_ratio:.2f}")
        if ts_attempts > 0:
            log_parts.append(f"ts={ts_successes}/{ts_completed}/{ts_attempts}")
        sps = step_samples / max(0.01, elapsed)
        mps = sps * current_sims
        log_parts.append(f"{sps:.0f}smp/s, {mps:.0f}mcts/s")
        log_parts.append(f"time={elapsed:.1f}s")
        logger.info(", ".join(log_parts))

        if eval_every > 0 and step % eval_every == 0:
            eval_model = current_model_path
            eval_sims = train_cfg.get("simulations", 200)
            h_temp = train_cfg.get("heuristic_temperature", 0.0)
            free_h_temp = train_cfg.get("free_heuristic_temperature", h_temp)
            eval_temp = train_cfg.get("eval_temperature", 0.0)
            benchmarks = eval_benchmarks or []

            for bench in benchmarks:
                if bench == "heuristic_constrained":
                    r = run_eval_vs_heuristic(
                        game_id, eval_model, eval_games, seed + step * 100000,
                        eval_sims, True, h_temp, max_workers)
                    logger.info(
                        f"  eval vs heuristic (constrained): win_rate={r['win_rate']:.1%} "
                        f"(W={r['wins']}, L={r['losses']}, D={r['draws']})")
                elif bench == "heuristic_free":
                    r = run_eval_vs_heuristic(
                        game_id, eval_model, eval_games, seed + step * 100000 + 50000,
                        eval_sims, False, free_h_temp, max_workers)
                    logger.info(
                        f"  eval vs heuristic (free): win_rate={r['win_rate']:.1%} "
                        f"(W={r['wins']}, L={r['losses']}, D={r['draws']})")
                else:
                    bench_result = run_eval_batch(
                        game_id, eval_model, bench,
                        eval_games, seed + step * 100000 + 90000,
                        eval_sims, eval_sims, max_workers,
                        max_game_plies=train_cfg.get("max_game_plies", 500),
                        temperature=eval_temp)
                    logger.info(
                        f"  eval vs {Path(bench).stem}: win_rate={bench_result['win_rate']:.1%} "
                        f"(W={bench_result['wins']}, L={bench_result['losses']}, D={bench_result['draws']})")

            # Gating: latest vs best (always runs)
            gating_result = run_eval_batch(
                game_id, eval_model, best_model_path,
                eval_games, seed + step * 100000 + 80000,
                eval_sims, eval_sims, max_workers,
                max_game_plies=train_cfg.get("max_game_plies", 500),
                temperature=eval_temp)
            gating_wr = gating_result["win_rate"]
            logger.info(
                f"  gating vs best: win_rate={gating_wr:.1%} "
                f"(W={gating_result['wins']}, L={gating_result['losses']}, D={gating_result['draws']})")
            gating_threshold = train_cfg.get("gating_accept_win_rate", 0.55)
            if gating_wr >= gating_threshold:
                best_onnx = models_dir / "model_best.onnx"
                shutil.copy2(eval_model, best_onnx)
                best_model_path = str(best_onnx)
                logger.info(f"  new best model: step {step}, win_rate={gating_wr:.1%}")

        checkpoint = {
            "step": step,
            "model_state_dict": net.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
        }
        torch.save(checkpoint, output_dir / "checkpoint.pt")

    logger.info(f"Training complete. Best model: {best_model_path}")
