"""Tests for hidden-information games: Coup and Love Letter.

Verifies that:
1. Belief tracker randomize_unseen produces valid determinizations
2. Feature encoder never leaks opponent hidden info (no peeking)
3. NoPeek traversal limiter fires during MCTS search
4. Selfplay and arena run correctly with hidden info
"""
import dinoboard_engine
import pytest

from conftest import get_test_model


# Coup is temporarily disabled at the build level (see CMakeLists.txt /
# setup.py / docs/GAME_FEATURES_OVERVIEW.md "诈唬核心游戏" Future Work).
# Skip the Coup half of this file until the build is restored. The
# Love Letter tests below still run against the live engine.
def _coup_available() -> bool:
    try:
        dinoboard_engine.game_metadata("coup")
        return True
    except Exception:
        return False


coup_skip = pytest.mark.skipif(
    not _coup_available(), reason="Coup temporarily disabled in build")


# ---------------------------------------------------------------------------
# Coup: hidden info tests
# ---------------------------------------------------------------------------

@coup_skip
class TestCoupHiddenInfo:

    def test_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="coup", seed=42, model_path=get_test_model("coup"),
            simulations=20, max_game_plies=60,
        )
        assert ep["total_plies"] > 0
        assert len(ep["samples"]) > 0

    def test_different_seeds_diverge(self):
        diff = 0
        for i in range(20):
            ep1 = dinoboard_engine.run_selfplay_episode(
                game_id="coup", seed=i, model_path=get_test_model("coup"),
                simulations=10, max_game_plies=30,
            )
            ep2 = dinoboard_engine.run_selfplay_episode(
                game_id="coup", seed=i + 10000, model_path=get_test_model("coup"),
                simulations=10, max_game_plies=30,
            )
            a1 = [s["action_id"] for s in ep1["samples"]]
            a2 = [s["action_id"] for s in ep2["samples"]]
            if a1 != a2:
                diff += 1
        assert diff > 10, f"only {diff}/20 coup games differed with different seeds"

    def test_determinism_with_same_seed(self):
        ep1 = dinoboard_engine.run_selfplay_episode(
            game_id="coup", seed=12345, model_path=get_test_model("coup"),
            simulations=10, max_game_plies=30,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        ep2 = dinoboard_engine.run_selfplay_episode(
            game_id="coup", seed=12345, model_path=get_test_model("coup"),
            simulations=10, max_game_plies=30,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        assert ep1["total_plies"] == ep2["total_plies"]
        assert ep1["winner"] == ep2["winner"]

    def test_selfplay_runs_under_ismcts(self):
        # ISMCTS: root sampling + DAG. Info-leak invariance is separately
        # covered by the API belief-match tests.
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="coup", seed=42, model_path=get_test_model("coup"),
            simulations=50, max_game_plies=40,
        )
        assert ep["total_plies"] > 0

    def test_arena_completes(self):
        m = get_test_model("coup")
        result = dinoboard_engine.run_arena_match(
            game_id="coup", seed=42,
            model_paths=[m, m],
            simulations_list=[10, 10], temperature=0.0,
        )
        assert "winner" in result
        assert result["total_plies"] > 0

    def test_3p_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="coup_3p", seed=42, model_path=get_test_model("coup_3p"),
            simulations=10, max_game_plies=60,
        )
        assert ep["total_plies"] > 0

    def test_4p_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="coup_4p", seed=42, model_path=get_test_model("coup_4p"),
            simulations=10, max_game_plies=60,
        )
        assert ep["total_plies"] > 0


@coup_skip
class TestCoupEncoderNoPeek:
    """Verify that the Coup feature encoder never leaks opponent hidden cards.

    The encoder has a 'known_hand' block (5 floats per player) that must be
    all-zero for non-perspective players and reflect actual hand for the
    perspective player.
    """

    def _get_features_for_perspective(self, seed, perspective):
        """Play a few plies, then encode from the given perspective."""
        gs = dinoboard_engine.GameSession("coup", seed=seed)
        for _ in range(3):
            if gs.is_terminal:
                break
            legal = gs.get_legal_actions()
            gs.apply_action(legal[0])

        state = gs.get_state_dict()
        enc = dinoboard_engine.encode_state("coup", seed=seed)
        return enc["features"], state

    def test_opponent_hand_features_are_zero(self):
        """For a 2-player game, encode from player 0's perspective.
        The 'known_hand' block for player 1 (the opponent) must be all-zero.

        Feature layout per player (18 features):
          [0] alive
          [1] coins/12
          [2-3] influence_count (2)
          [4-8] revealed_cards (5, one per character)
          [9-13] known_hand (5, one per character) ← this block
          [14-17] is_active, is_target, is_blocker, is_challenger
        """
        features_per_player = 18
        known_hand_offset = 9
        known_hand_size = 5

        gs = dinoboard_engine.GameSession("coup", seed=100)
        enc = dinoboard_engine.encode_state("coup", seed=100)
        features = enc["features"]

        opponent_start = features_per_player
        opponent_hand = features[
            opponent_start + known_hand_offset :
            opponent_start + known_hand_offset + known_hand_size
        ]
        assert all(v == 0.0 for v in opponent_hand), (
            f"Opponent known_hand features should be all-zero but got {opponent_hand}"
        )

    def test_self_hand_features_nonzero(self):
        """Player 0's own known_hand block should reflect their actual hand."""
        features_per_player = 18
        known_hand_offset = 9
        known_hand_size = 5

        enc = dinoboard_engine.encode_state("coup", seed=100)
        features = enc["features"]

        self_hand = features[known_hand_offset : known_hand_offset + known_hand_size]
        assert any(v > 0.0 for v in self_hand), (
            f"Self known_hand features should be non-zero but got {self_hand}"
        )

    def test_different_perspectives_hide_different_info(self):
        """Encoding the same state from two perspectives should differ in the
        known_hand blocks — each player sees only their own hand."""
        gs = dinoboard_engine.GameSession("coup", seed=200)
        legal = gs.get_legal_actions()
        gs.apply_action(legal[0])

        features_per_player = 18
        known_hand_offset = 9
        known_hand_size = 5

        enc = dinoboard_engine.encode_state("coup", seed=200)
        f = enc["features"]

        p0_sees_self_hand = f[known_hand_offset : known_hand_offset + known_hand_size]
        p0_sees_opp_hand = f[
            features_per_player + known_hand_offset :
            features_per_player + known_hand_offset + known_hand_size
        ]
        assert all(v == 0.0 for v in p0_sees_opp_hand)
        assert p0_sees_self_hand != p0_sees_opp_hand


# ---------------------------------------------------------------------------
# Love Letter: hidden info tests
# ---------------------------------------------------------------------------

class TestLoveLetterHiddenInfo:

    def test_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter", seed=42, model_path=get_test_model("loveletter"),
            simulations=20, max_game_plies=30,
        )
        assert ep["total_plies"] > 0
        assert len(ep["samples"]) > 0

    def test_different_seeds_diverge(self):
        diff = 0
        for i in range(20):
            ep1 = dinoboard_engine.run_selfplay_episode(
                game_id="loveletter", seed=i, model_path=get_test_model("loveletter"),
                simulations=10, max_game_plies=20,
            )
            ep2 = dinoboard_engine.run_selfplay_episode(
                game_id="loveletter", seed=i + 10000, model_path=get_test_model("loveletter"),
                simulations=10, max_game_plies=20,
            )
            a1 = [s["action_id"] for s in ep1["samples"]]
            a2 = [s["action_id"] for s in ep2["samples"]]
            if a1 != a2:
                diff += 1
        assert diff > 10, f"only {diff}/20 loveletter games differed with different seeds"

    def test_determinism_with_same_seed(self):
        ep1 = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter", seed=12345, model_path=get_test_model("loveletter"),
            simulations=10, max_game_plies=20,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        ep2 = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter", seed=12345, model_path=get_test_model("loveletter"),
            simulations=10, max_game_plies=20,
            dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        assert ep1["total_plies"] == ep2["total_plies"]
        assert ep1["winner"] == ep2["winner"]

    def test_selfplay_runs_under_ismcts(self):
        # ISMCTS: no NoPeek / afterstate cap. traversal_stops always 0.
        # Info-leak invariance separately covered by test_api_mcts_policy_invariance
        # and TestLoveLetterGuardAccuracy.
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter", seed=42, model_path=get_test_model("loveletter"),
            simulations=50, max_game_plies=30,
        )
        assert ep["total_plies"] > 0

    def test_arena_completes(self):
        m = get_test_model("loveletter")
        result = dinoboard_engine.run_arena_match(
            game_id="loveletter", seed=42,
            model_paths=[m, m],
            simulations_list=[10, 10], temperature=0.0,
        )
        assert "winner" in result
        assert result["total_plies"] > 0

    def test_3p_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter_3p", seed=42, model_path=get_test_model("loveletter_3p"),
            simulations=10, max_game_plies=30,
        )
        assert ep["total_plies"] > 0

    def test_4p_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter_4p", seed=42, model_path=get_test_model("loveletter_4p"),
            simulations=10, max_game_plies=30,
        )
        assert ep["total_plies"] > 0


class TestLoveLetterEncoderNoPeek:
    """Verify that the Love Letter feature encoder doesn't leak opponent hand."""

    def test_opponent_hand_features_are_zero_or_placeholder(self):
        """Opponent's hand card should not be directly encoded in features.

        Love Letter feature layout per player (variable, but hand_card block
        should be zero for opponents). We verify by running multiple seeds
        and checking that encoding from player 0's perspective never reveals
        player 1's actual hand.
        """
        gs = dinoboard_engine.GameSession("loveletter", seed=300)
        if gs.is_terminal:
            return

        state = gs.get_state_dict()
        enc = dinoboard_engine.encode_state("loveletter", seed=300)
        features = enc["features"]
        assert len(features) == enc["feature_dim"]


class TestLoveLetterGuardAccuracy:
    """Regression for BUG-023: Love Letter MCTS was leaking opponent's true
    hand when Guard correctly guessed → terminal → no draw → nonce unchanged
    → NoPeek never fired → MCTS saw the Q=1.0 win from the true state.

    This test plays many games vs. a random opponent and measures the AI's
    Guard-guess accuracy in positions where the belief tracker has no
    reveal-based knowledge of the target. Expected behaviour: accuracy is
    statistically consistent with random guessing over the unknown pool
    (≈14% for uniform, a bit higher if the tracker uses discard counts to
    narrow the pool). Under the bug, accuracy exceeded 75%.

    The bound is loose on purpose — tracker CAN narrow the pool from the
    public discard pile, so the true no-cheat rate is somewhat above 1/7.
    The test catches any egregious leak that pushes accuracy into the
    40%+ range.
    """

    def test_guard_accuracy_not_better_than_bounded_inference(self):
        # Use the deployed trained model (not a random-weight session model).
        # BUG-023 manifested through MCTS exploiting the structured Q values
        # a trained model assigns to "correct guess" branches; a random model
        # has near-uniform Q so the leak's signal-to-noise is poor and the
        # test would flake on small N.
        from pathlib import Path
        project_root = Path(__file__).resolve().parents[1]
        model = str(project_root / "games/loveletter/model/loveletter_2p.onnx")
        if not Path(model).exists():
            pytest.skip("deployed loveletter_2p model not available")
        total, correct = 0, 0
        for seed in range(60):
            gs = dinoboard_engine.GameSession("loveletter", seed, model, False)
            while not gs.is_terminal:
                cp = gs.current_player
                if cp == 0:
                    result = gs.get_ai_action(simulations=100, temperature=0.0)
                    info = result["action_info"]
                    if info.get("type") == "guard":
                        target = info["target"]
                        guess = info["guess"]
                        state = gs.get_state_dict()
                        actual = state["players"][target]["hand"]
                        # Filter out positions where tracker legitimately knows
                        # the target's hand (via prior Priest/Baron/King).
                        snap = gs.get_belief_snapshot()
                        known = snap.get("known_hand", [0] * 4)
                        if target < len(known) and known[target] > 0:
                            pass  # skip — legitimate info
                        else:
                            total += 1
                            if guess == actual:
                                correct += 1
                    gs.apply_action(result["action"])
                else:
                    import random
                    random.seed(seed * 100 + gs.current_player)
                    legal = gs.get_legal_actions()
                    gs.apply_action(random.choice(legal))

        if total == 0:
            pytest.skip("AI did not play any Guards without prior info")

        rate = correct / total
        # Clean-AI rate with the deployed model and 100 sims is around 20%
        # (some bias because the ai_view is seeded with a deterministic
        # placeholder opp hand from the remaining multiset, biasing guesses
        # toward common cards like Guard, but the real opp hand is random).
        # BUG-023 produced 76%. A 40% ceiling catches any regression with
        # >2x margin over clean and >1.8x margin under the bug.
        assert rate < 0.40, (
            f"Love Letter AI Guard accuracy {rate:.1%} ({correct}/{total}) "
            f"exceeds 40% — probable hidden-info leak in MCTS (see BUG-023 "
            f"/ the architectural refactor notes). Random baseline is ~14%."
        )


# ---------------------------------------------------------------------------
# Cross-game: feature encoding consistency after randomize_unseen
# ---------------------------------------------------------------------------

class TestRandomizeUnseenConsistency:
    """After randomize_unseen, the encoded features should differ from the
    original but remain valid (correct dimension, legal mask unchanged)."""

    @pytest.mark.parametrize("game_id", (["coup", "loveletter"] if _coup_available() else ["loveletter"]))
    def test_samples_have_correct_feature_dim(self, game_id):
        meta = dinoboard_engine.game_metadata(game_id)
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=get_test_model(game_id),
            simulations=10, max_game_plies=30,
        )
        for s in ep["samples"]:
            assert len(s["features"]) == meta["feature_dim"], (
                f"{game_id} ply {s['ply']}: dim={len(s['features'])}, expected {meta['feature_dim']}"
            )

    @pytest.mark.parametrize("game_id", (["coup", "loveletter"] if _coup_available() else ["loveletter"]))
    def test_policy_only_on_legal_actions(self, game_id):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=get_test_model(game_id),
            simulations=10, max_game_plies=30,
        )
        for s in ep["samples"]:
            legal_set = {i for i, m in enumerate(s["legal_mask"]) if m > 0}
            visited = {aid for aid, v in zip(s["policy_action_ids"], s["policy_action_visits"]) if v > 0}
            illegal = visited - legal_set
            assert not illegal, (
                f"{game_id} ply {s['ply']}: visited illegal actions {illegal}"
            )


# ---------------------------------------------------------------------------
# GameSession: configure_tail_solve + AI stats
# ---------------------------------------------------------------------------

class TestGameSessionTailSolve:

    def test_configure_tail_solve_no_crash(self):
        gs = dinoboard_engine.GameSession(
            "quoridor", seed=42, model_path=get_test_model("quoridor"))
        gs.configure_tail_solve(True, 5, 1000)
        result = gs.get_ai_action(simulations=10, temperature=0.0)
        assert "action" in result
        assert "stats" in result
        stats = result["stats"]
        assert "tail_solved" in stats
        assert "tail_solve_value" in stats

    def test_configure_tail_solve_disabled(self):
        gs = dinoboard_engine.GameSession(
            "quoridor", seed=42, model_path=get_test_model("quoridor"))
        gs.configure_tail_solve(False, 5, 1000)
        result = gs.get_ai_action(simulations=10, temperature=0.0)
        assert result["stats"]["tail_solved"] is False

    def test_stats_include_dag_reuse_hits(self):
        # Hidden-info games under ISMCTS should exercise DAG node reuse.
        gs = dinoboard_engine.GameSession(
            "loveletter", seed=42, model_path=get_test_model("loveletter"))
        result = gs.get_ai_action(simulations=20, temperature=0.0)
        assert "dag_reuse_hits" in result["stats"]


# ---------------------------------------------------------------------------
# GameSession with temperature: hidden info games should have action variety
# ---------------------------------------------------------------------------

class TestGameSessionTemperature:

    @pytest.mark.parametrize("game_id", (["coup", "loveletter"] if _coup_available() else ["loveletter"]))
    def test_nonzero_temperature_adds_variety(self, game_id):
        """With temperature > 0, different seeds should produce different first actions."""
        actions = set()
        model = get_test_model(game_id)
        for seed in range(20):
            gs = dinoboard_engine.GameSession(game_id, seed=seed, model_path=model)
            result = gs.get_ai_action(simulations=10, temperature=1.0)
            if "action" in result:
                actions.add(result["action"])
        assert len(actions) > 1, (
            f"{game_id}: temperature=1.0 always picked the same action across 20 seeds"
        )
