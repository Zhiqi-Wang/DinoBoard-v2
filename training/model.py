"""Neural network model for policy-value prediction."""
from __future__ import annotations

from pathlib import Path

import torch
import torch.nn as nn


class PVNet(nn.Module):
    def __init__(self, input_dim: int, policy_dim: int, hidden_layers: list[int]):
        super().__init__()
        layers: list[nn.Module] = []
        prev = input_dim
        for h in hidden_layers:
            layers.extend([nn.Linear(prev, h), nn.ReLU()])
            prev = h
        self.backbone = nn.Sequential(*layers)
        self.policy_head = nn.Linear(prev, policy_dim)
        self.value_head = nn.Sequential(nn.Linear(prev, 1), nn.Tanh())

    def forward(self, x: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        h = self.backbone(x)
        return self.policy_head(h), self.value_head(h)


def create_model_from_config(game_config: dict) -> PVNet:
    input_dim = game_config["feature_dim"]
    policy_dim = game_config["action_space"]
    hidden_layers = game_config.get("network", {}).get("hidden_layers", [256, 256])
    return PVNet(input_dim, policy_dim, hidden_layers)


def export_onnx(net: PVNet, path: Path, input_dim: int) -> str:
    net.eval()
    path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, input_dim), dtype=torch.float32)
    torch.onnx.export(
        net, dummy, str(path),
        input_names=["features"],
        output_names=["policy", "value"],
        dynamic_axes={
            "features": {0: "batch"},
            "policy": {0: "batch"},
            "value": {0: "batch"},
        },
        opset_version=13,
    )
    return str(path)
