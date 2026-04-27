"""Unified training CLI: python -m training.cli --game splendor --output runs/splendor_001"""
from __future__ import annotations

import argparse
import json
import logging
import sys
from pathlib import Path

from .pipeline import run_training_loop


def find_game_config(game_id: str) -> dict:
    root = Path(__file__).resolve().parents[1]
    config_path = root / "games" / game_id / "config" / "game.json"
    if not config_path.exists():
        raise FileNotFoundError(f"Game config not found: {config_path}")
    with open(config_path, "r") as f:
        return json.load(f)


def main() -> int:
    parser = argparse.ArgumentParser(description="DinoBoard training CLI")
    parser.add_argument("--game", type=str, required=True, help="Game ID (e.g., tictactoe, splendor)")
    parser.add_argument("--output", type=str, required=True, help="Output directory")
    parser.add_argument("--steps", type=int, default=0, help="Override training steps (0 = use config)")
    parser.add_argument("--episodes", type=int, default=0, help="Override episodes per step (0 = use config)")
    parser.add_argument("--workers", type=int, default=4, help="Number of parallel workers")
    parser.add_argument("--seed", type=int, default=20260323, help="Random seed")
    parser.add_argument("--eval-every", type=int, default=50, help="Evaluate every N steps (0 = disable)")
    parser.add_argument("--eval-games", type=int, default=40, help="Number of evaluation games")
    parser.add_argument("--batch-size", type=int, default=0, help="Training batch size (0 = use config)")
    parser.add_argument("--lr", type=float, default=0.0, help="Learning rate (0 = use config)")
    parser.add_argument("--log-level", type=str, default="INFO", help="Logging level")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    game_config = find_game_config(args.game)
    train_cfg = game_config.get("training", {})

    steps = args.steps if args.steps > 0 else train_cfg.get("steps", 1000)
    episodes = args.episodes if args.episodes > 0 else train_cfg.get("episodes_per_step", 200)
    batch_size = args.batch_size if args.batch_size > 0 else train_cfg.get("batch_size", 512)
    lr = args.lr if args.lr > 0 else train_cfg.get("learning_rate", 0.001)

    run_training_loop(
        game_id=args.game,
        game_config=game_config,
        output_dir=Path(args.output),
        steps=steps,
        episodes_per_step=episodes,
        eval_every=args.eval_every,
        eval_games=args.eval_games,
        max_workers=args.workers,
        batch_size=batch_size,
        learning_rate=lr,
        seed=args.seed,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
