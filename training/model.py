"""Neural network model for policy-value prediction."""
from __future__ import annotations

from pathlib import Path

import torch
import torch.nn as nn


class PVNet(nn.Module):
    def __init__(self, input_dim: int, policy_dim: int, hidden_layers: list[int],
                 auxiliary_score: bool = False, num_players: int = 2):
        super().__init__()
        self.num_players = num_players
        layers: list[nn.Module] = []
        prev = input_dim
        for h in hidden_layers:
            layers.extend([nn.Linear(prev, h), nn.ReLU()])
            prev = h
        self.backbone = nn.Sequential(*layers)
        self.policy_head = nn.Linear(prev, policy_dim)
        self.value_head = nn.Sequential(nn.Linear(prev, num_players), nn.Tanh())
        self.has_score_head = auxiliary_score
        if auxiliary_score:
            self.score_head = nn.Sequential(nn.Linear(prev, 1), nn.Tanh())

    def forward(self, x: torch.Tensor):
        h = self.backbone(x)
        if self.has_score_head:
            return self.policy_head(h), self.value_head(h), self.score_head(h)
        return self.policy_head(h), self.value_head(h)


def create_model_from_config(game_config: dict) -> PVNet:
    input_dim = game_config["feature_dim"]
    policy_dim = game_config["action_space"]
    hidden_layers = game_config.get("network", {}).get("hidden_layers", [256, 256])
    auxiliary_score = game_config.get("training", {}).get("auxiliary_score", False)
    if "num_players" not in game_config:
        raise KeyError(
            "game_config missing 'num_players'. Inject from engine.game_metadata(game_id) "
            "before calling create_model_from_config — JSON 'players.max' is not authoritative."
        )
    num_players = game_config["num_players"]
    return PVNet(input_dim, policy_dim, hidden_layers,
                 auxiliary_score=auxiliary_score, num_players=num_players)


def export_onnx(net: PVNet, path: Path, input_dim: int) -> str:
    net.eval()
    path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, input_dim), dtype=torch.float32)

    output_names = ["policy", "value"]
    dynamic_axes = {
        "features": {0: "batch"},
        "policy": {0: "batch"},
        "value": {0: "batch"},
    }
    if net.has_score_head:
        output_names.append("score")
        dynamic_axes["score"] = {0: "batch"}

    torch.onnx.export(
        net, dummy, str(path),
        input_names=["features"],
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        opset_version=13,
    )
    return str(path)
