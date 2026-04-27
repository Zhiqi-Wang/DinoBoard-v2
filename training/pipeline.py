"""Self-play training pipeline: selfplay -> collect samples -> train -> export -> repeat."""
from __future__ import annotations

import json
import logging
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any

import torch

from .model import PVNet, create_model_from_config, export_onnx

logger = logging.getLogger(__name__)


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
    )


def _worker_arena(args: tuple) -> dict[str, Any]:
    """Run a single arena match in a worker process."""
    import dinoboard_engine
    game_id, seed, model_0, model_1, sims_0, sims_1 = args
    return dinoboard_engine.run_arena_match(
        game_id=game_id, seed=seed,
        model_path_0=model_0, model_path_1=model_1,
        simulations_0=sims_0, simulations_1=sims_1,
        temperature=0.0,
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


def collect_training_data(
    episodes: list[dict[str, Any]],
    action_space: int,
) -> tuple[list[list[float]], list[list[float]], list[float]]:
    """Extract (features, policy_targets, value_targets) from selfplay episodes."""
    import dinoboard_engine

    all_features: list[list[float]] = []
    all_policies: list[list[float]] = []
    all_values: list[float] = []

    for ep in episodes:
        for sample in ep.get("samples", []):
            policy = normalize_policy(
                sample.get("policy_action_ids", []),
                sample.get("policy_action_visits", []),
                action_space,
            )
            all_policies.append(policy)
            all_values.append(float(sample.get("z", 0.0)))
            all_features.append([])  # placeholder, filled from encode

    return all_features, all_policies, all_values


def train_step(
    net: PVNet,
    optimizer: torch.optim.Optimizer,
    features: torch.Tensor,
    policy_targets: torch.Tensor,
    value_targets: torch.Tensor,
) -> dict[str, float]:
    net.train()
    policy_logits, value_pred = net(features)
    value_loss = torch.nn.functional.mse_loss(value_pred.squeeze(-1), value_targets)
    policy_loss = -(policy_targets * torch.nn.functional.log_softmax(policy_logits, dim=-1)).sum(dim=-1).mean()
    loss = policy_loss + value_loss
    optimizer.zero_grad()
    loss.backward()
    optimizer.step()
    return {
        "loss": loss.item(),
        "policy_loss": policy_loss.item(),
        "value_loss": value_loss.item(),
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


def run_eval_batch(
    game_id: str,
    candidate_path: str,
    opponent_path: str,
    num_games: int,
    base_seed: int,
    sims_candidate: int,
    sims_opponent: int,
    max_workers: int,
) -> dict[str, Any]:
    tasks = []
    for i in range(num_games):
        if i % 2 == 0:
            tasks.append((game_id, base_seed + i, candidate_path, opponent_path, sims_candidate, sims_opponent))
        else:
            tasks.append((game_id, base_seed + i, opponent_path, candidate_path, sims_opponent, sims_candidate))

    wins = 0
    losses = 0
    draws = 0
    with ProcessPoolExecutor(max_workers=max_workers) as pool:
        for idx, r in enumerate(pool.map(_worker_arena, tasks)):
            is_swapped = (idx % 2 == 1)
            w = r["winner"]
            if r["draw"] or w < 0:
                draws += 1
            elif (w == 0 and not is_swapped) or (w == 1 and is_swapped):
                wins += 1
            else:
                losses += 1

    total = wins + losses + draws
    win_rate = wins / max(1, total)
    return {"wins": wins, "losses": losses, "draws": draws, "win_rate": win_rate}


def run_training_loop(
    game_id: str,
    game_config: dict,
    output_dir: Path,
    steps: int = 1000,
    episodes_per_step: int = 200,
    eval_every: int = 50,
    eval_games: int = 40,
    max_workers: int = 4,
    batch_size: int = 512,
    learning_rate: float = 0.001,
    seed: int = 20260323,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    models_dir = output_dir / "models"
    models_dir.mkdir(exist_ok=True)

    train_cfg = game_config.get("training", {})
    action_space = game_config["action_space"]
    feature_dim = game_config["feature_dim"]

    net = create_model_from_config(game_config)
    torch.manual_seed(seed)
    optimizer = torch.optim.Adam(net.parameters(), lr=learning_rate)

    initial_onnx = models_dir / "model_init.onnx"
    export_onnx(net, initial_onnx, feature_dim)
    current_model_path = str(initial_onnx)
    best_model_path = current_model_path

    logger.info(f"Starting training: game={game_id}, steps={steps}, episodes/step={episodes_per_step}")

    replay_buffer: list[tuple[list[float], list[float], float]] = []
    max_buffer_size = episodes_per_step * 50 * 20

    for step in range(1, steps + 1):
        t0 = time.perf_counter()

        selfplay_cfg = {
            "simulations": train_cfg.get("simulations", 200),
            "c_puct": train_cfg.get("c_puct", 1.4),
            "temperature": train_cfg.get("temperature", 1.0),
            "dirichlet_alpha": train_cfg.get("dirichlet_alpha", 0.3),
            "dirichlet_epsilon": train_cfg.get("dirichlet_epsilon", 0.25),
            "dirichlet_on_first_n_plies": train_cfg.get("dirichlet_on_first_n_plies", 30),
            "max_game_plies": train_cfg.get("max_game_plies", 500),
        }

        episodes = run_selfplay_batch(
            game_id, current_model_path, episodes_per_step,
            seed + step * 10000, selfplay_cfg, max_workers)

        new_samples = []
        for ep in episodes:
            for sample in ep.get("samples", []):
                policy = normalize_policy(
                    sample.get("policy_action_ids", []),
                    sample.get("policy_action_visits", []),
                    action_space,
                )
                new_samples.append((
                    [],  # features placeholder — we re-encode during training
                    policy,
                    float(sample.get("z", 0.0)),
                ))
        replay_buffer.extend(new_samples)
        if len(replay_buffer) > max_buffer_size:
            replay_buffer = replay_buffer[-max_buffer_size:]

        winners = [ep["winner"] for ep in episodes]
        p0_wins = sum(1 for w in winners if w == 0)
        p1_wins = sum(1 for w in winners if w == 1)
        draws_count = sum(1 for w in winners if w < 0)

        import dinoboard_engine
        all_features = []
        all_policies = []
        all_values = []
        for ep in episodes:
            for sample in ep.get("samples", []):
                enc = dinoboard_engine.encode_state(game_id, seed + sample["ply"])
                features = enc["features"]
                policy = normalize_policy(
                    sample.get("policy_action_ids", []),
                    sample.get("policy_action_visits", []),
                    action_space,
                )
                all_features.append(features)
                all_policies.append(policy)
                all_values.append(float(sample.get("z", 0.0)))

        if not all_features:
            logger.warning(f"Step {step}: no training samples, skipping")
            continue

        feat_tensor = torch.tensor(all_features, dtype=torch.float32)
        policy_tensor = torch.tensor(all_policies, dtype=torch.float32)
        value_tensor = torch.tensor(all_values, dtype=torch.float32)

        n = feat_tensor.size(0)
        num_batches = max(1, n // batch_size)
        perm = torch.randperm(n)
        total_loss = 0.0
        for b in range(num_batches):
            idx = perm[b * batch_size: (b + 1) * batch_size]
            metrics = train_step(
                net, optimizer,
                feat_tensor[idx], policy_tensor[idx], value_tensor[idx])
            total_loss += metrics["loss"]
        avg_loss = total_loss / max(1, num_batches)

        step_onnx = models_dir / f"model_step_{step:05d}.onnx"
        export_onnx(net, step_onnx, feature_dim)
        current_model_path = str(step_onnx)

        elapsed = time.perf_counter() - t0
        logger.info(
            f"Step {step}/{steps}: loss={avg_loss:.4f}, "
            f"episodes={len(episodes)}, samples={n}, "
            f"p0_wins={p0_wins}, p1_wins={p1_wins}, draws={draws_count}, "
            f"time={elapsed:.1f}s"
        )

        if eval_every > 0 and step % eval_every == 0:
            eval_result = run_eval_batch(
                game_id, current_model_path, best_model_path,
                eval_games, seed + step * 100000,
                train_cfg.get("simulations", 200),
                train_cfg.get("simulations", 200),
                max_workers)
            win_rate = eval_result["win_rate"]
            logger.info(
                f"  eval benchmark: win_rate={win_rate:.1%} "
                f"(W={eval_result['wins']}, L={eval_result['losses']}, D={eval_result['draws']})"
            )
            if win_rate >= 0.55:
                best_model_path = current_model_path
                logger.info(f"  new best model: {best_model_path}")

        checkpoint = {
            "step": step,
            "model_state_dict": net.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
        }
        torch.save(checkpoint, output_dir / "checkpoint.pt")

    logger.info(f"Training complete. Best model: {best_model_path}")
