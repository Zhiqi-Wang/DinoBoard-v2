"""Tests for web.json config loading and session creation with web config.

Verifies that:
1. load_web_configs() correctly reads web.json files
2. Fallback to game.json legacy fields works
3. Difficulty overrides apply correctly
4. Tail solve config propagates to GameSession
5. Action filter config propagates correctly
"""
import json
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))
sys.path.insert(0, str(PROJECT_ROOT / "platform"))


# ---------------------------------------------------------------------------
# load_web_configs: reads real web.json files
# ---------------------------------------------------------------------------

class TestLoadWebConfigs:

    def test_loads_existing_web_configs(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        assert "coup" in configs
        assert "loveletter" in configs
        assert "quoridor" in configs

    def test_coup_has_analysis_simulations(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        assert configs["coup"]["analysis_simulations"] == 2000

    def test_coup_has_difficulty_overrides(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        overrides = configs["coup"]["difficulty_overrides"]
        assert "casual" in overrides
        assert "expert" in overrides
        assert overrides["casual"]["simulations"] == 50
        assert overrides["casual"]["temperature"] == 0.3
        assert overrides["expert"]["temperature"] == 0.1

    def test_loveletter_has_temperature_overrides(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        overrides = configs["loveletter"]["difficulty_overrides"]
        assert overrides["casual"]["temperature"] == 0.3
        assert overrides["expert"]["temperature"] == 0.1

    def test_quoridor_has_action_filter(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        assert configs["quoridor"]["ai_use_action_filter"] is True

    def test_quoridor_has_tail_solve(self):
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        ts = configs["quoridor"]["tail_solve"]
        assert ts["enabled"] is True
        assert ts["depth_limit"] == 10
        assert ts["node_budget"] == 200000

    def test_games_without_web_json_not_in_configs(self):
        """Games that have no web.json and no legacy web fields should not appear."""
        from game_service.sessions import load_web_configs
        configs = load_web_configs()
        assert "tictactoe" not in configs


# ---------------------------------------------------------------------------
# Difficulty preset + override resolution
# ---------------------------------------------------------------------------

class TestDifficultyResolution:

    def test_coup_casual_uses_override_simulations(self):
        from game_service.sessions import WEB_CONFIGS, DIFFICULTY_PRESETS
        web_cfg = WEB_CONFIGS.get("coup", {})
        preset = DIFFICULTY_PRESETS["casual"]
        diff_overrides = web_cfg.get("difficulty_overrides", {}).get("casual", {})
        effective_sims = diff_overrides.get("simulations", preset["simulations"])
        assert effective_sims == 50

    def test_coup_casual_uses_override_temperature(self):
        from game_service.sessions import WEB_CONFIGS, DIFFICULTY_PRESETS
        web_cfg = WEB_CONFIGS.get("coup", {})
        preset = DIFFICULTY_PRESETS["casual"]
        diff_overrides = web_cfg.get("difficulty_overrides", {}).get("casual", {})
        effective_temp = diff_overrides.get("temperature", preset["temperature"])
        assert effective_temp == 0.3

    def test_coup_expert_uses_override_temperature(self):
        from game_service.sessions import WEB_CONFIGS, DIFFICULTY_PRESETS
        web_cfg = WEB_CONFIGS.get("coup", {})
        preset = DIFFICULTY_PRESETS["expert"]
        diff_overrides = web_cfg.get("difficulty_overrides", {}).get("expert", {})
        effective_temp = diff_overrides.get("temperature", preset["temperature"])
        assert effective_temp == 0.1

    def test_loveletter_casual_inherits_default_simulations(self):
        """Love Letter casual has temperature override but no simulations override."""
        from game_service.sessions import WEB_CONFIGS, DIFFICULTY_PRESETS
        web_cfg = WEB_CONFIGS.get("loveletter", {})
        preset = DIFFICULTY_PRESETS["casual"]
        diff_overrides = web_cfg.get("difficulty_overrides", {}).get("casual", {})
        effective_sims = diff_overrides.get("simulations", preset["simulations"])
        assert effective_sims == preset["simulations"]

    def test_heuristic_preset_no_model(self):
        from game_service.sessions import DIFFICULTY_PRESETS
        assert DIFFICULTY_PRESETS["heuristic"]["use_model"] is False

    def test_analysis_simulations_default(self):
        """Games without analysis_simulations should use default 5000."""
        from game_service.sessions import WEB_CONFIGS
        web_cfg = WEB_CONFIGS.get("quoridor", {})
        analysis_sims = web_cfg.get("analysis_simulations", 5000)
        assert analysis_sims == 5000

    def test_analysis_simulations_coup(self):
        from game_service.sessions import WEB_CONFIGS
        web_cfg = WEB_CONFIGS.get("coup", {})
        assert web_cfg.get("analysis_simulations", 5000) == 2000


# ---------------------------------------------------------------------------
# Session creation: verify config flows into session dict
# ---------------------------------------------------------------------------

class TestSessionCreation:

    @staticmethod
    def _ensure_test_model(game_id):
        """Create a random model if none exists, so create_session doesn't fail."""
        model_dir = PROJECT_ROOT / "games" / game_id / "model"
        model_path = model_dir / "best_model.onnx"
        if model_path.exists():
            return
        sys.path.insert(0, str(PROJECT_ROOT))
        from conftest import get_test_model
        import shutil
        src = get_test_model(game_id)
        model_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, model_path)

    def _create_session(self, game_id, difficulty):
        from game_service.sessions import create_session
        import random
        if difficulty != "heuristic":
            self._ensure_test_model(game_id)
        seed = random.randint(0, 2**31)
        session_id, sess = create_session(
            game_id=game_id, seed=seed,
            human_player=0, num_players=2, difficulty=difficulty,
        )
        return session_id, sess

    def test_coup_casual_session_has_correct_simulations(self):
        _, sess = self._create_session("coup", "casual")
        assert sess["simulations"] == 50

    def test_coup_casual_session_has_correct_temperature(self):
        _, sess = self._create_session("coup", "casual")
        assert sess["temperature"] == 0.3

    def test_coup_expert_session_has_correct_temperature(self):
        _, sess = self._create_session("coup", "expert")
        assert sess["temperature"] == 0.1

    def test_coup_session_has_correct_analysis_sims(self):
        _, sess = self._create_session("coup", "casual")
        assert sess["analysis_simulations"] == 2000

    def test_quoridor_session_has_filter_enabled(self):
        _, sess = self._create_session("quoridor", "heuristic")
        assert sess["ai_use_filter"] is True

    def test_tictactoe_session_no_filter(self):
        _, sess = self._create_session("tictactoe", "heuristic")
        assert sess["ai_use_filter"] is False

    def test_loveletter_expert_has_temperature(self):
        _, sess = self._create_session("loveletter", "expert")
        assert sess["temperature"] == 0.1

    def test_unknown_difficulty_raises(self):
        from game_service.sessions import create_session
        with pytest.raises(ValueError, match="unknown difficulty"):
            create_session("tictactoe", seed=1, human_player=0,
                           num_players=2, difficulty="impossible")

    def test_heuristic_session_no_model(self):
        _, sess = self._create_session("coup", "heuristic")
        assert sess["use_model"] is False


# ---------------------------------------------------------------------------
# Backward compatibility: fallback to game.json legacy fields
# ---------------------------------------------------------------------------

class TestWebConfigFallback:

    def test_fallback_reads_legacy_web_field(self, tmp_path):
        """If web.json doesn't exist, load_web_configs falls back to game.json web field."""
        games_dir = tmp_path / "games" / "fakegame" / "config"
        games_dir.mkdir(parents=True)
        game_json = {
            "game_id": "fakegame",
            "web": {
                "analysis_simulations": 3000,
                "difficulty_overrides": {"casual": {"temperature": 0.5}},
            },
        }
        (games_dir / "game.json").write_text(json.dumps(game_json))

        from game_service.sessions import load_web_configs
        with patch("game_service.sessions.PROJECT_ROOT", tmp_path):
            configs = load_web_configs()
        assert "fakegame" in configs
        assert configs["fakegame"]["analysis_simulations"] == 3000
        assert configs["fakegame"]["difficulty_overrides"]["casual"]["temperature"] == 0.5

    def test_fallback_reads_legacy_action_filter(self, tmp_path):
        """Legacy ai_use_action_filter in game.json should be picked up."""
        games_dir = tmp_path / "games" / "fakegame2" / "config"
        games_dir.mkdir(parents=True)
        game_json = {
            "game_id": "fakegame2",
            "ai_use_action_filter": True,
        }
        (games_dir / "game.json").write_text(json.dumps(game_json))

        from game_service.sessions import load_web_configs
        with patch("game_service.sessions.PROJECT_ROOT", tmp_path):
            configs = load_web_configs()
        assert configs["fakegame2"]["ai_use_action_filter"] is True

    def test_web_json_takes_priority_over_legacy(self, tmp_path):
        """If both web.json and game.json web field exist, web.json wins."""
        games_dir = tmp_path / "games" / "fakegame3" / "config"
        games_dir.mkdir(parents=True)
        game_json = {
            "game_id": "fakegame3",
            "web": {"analysis_simulations": 9999},
        }
        web_json = {"analysis_simulations": 1234}
        (games_dir / "game.json").write_text(json.dumps(game_json))
        (games_dir / "web.json").write_text(json.dumps(web_json))

        from game_service.sessions import load_web_configs
        with patch("game_service.sessions.PROJECT_ROOT", tmp_path):
            configs = load_web_configs()
        assert configs["fakegame3"]["analysis_simulations"] == 1234

    def test_no_config_returns_empty(self, tmp_path):
        """Game with no web.json and no legacy fields should not appear."""
        games_dir = tmp_path / "games" / "minimal" / "config"
        games_dir.mkdir(parents=True)
        (games_dir / "game.json").write_text(json.dumps({"game_id": "minimal"}))

        from game_service.sessions import load_web_configs
        with patch("game_service.sessions.PROJECT_ROOT", tmp_path):
            configs = load_web_configs()
        assert "minimal" not in configs
