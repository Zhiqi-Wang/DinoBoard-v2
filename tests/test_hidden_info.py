"""Tests for hidden-information games: NoPeek, belief tracking, stochastic detection.

Splendor and Azul both have hidden info (tile bag). The NoPeek system prevents
MCTS from "peeking" at hidden state by stopping traversal at stochastic
transitions and using belief-tracker-based determinizations.
"""
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, get_test_model


# ---------------------------------------------------------------------------
# Splendor: full NoPeek support (chance sampling enabled)
# ---------------------------------------------------------------------------

class TestSplendorHiddenInfo:

    def test_selfplay_completes_with_hidden_info(self):
        """Splendor selfplay should handle hidden info without crash."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=42, model_path=get_test_model("splendor"), simulations=20,
            max_game_plies=60,
        )
        assert ep["total_plies"] > 0
        assert len(ep["samples"]) > 0

    def test_different_seeds_produce_different_games(self):
        """Hidden info randomization should make different seeds diverge."""
        diff = 0
        for i in range(20):
            ep1 = dinoboard_engine.run_selfplay_episode(
                game_id="splendor", seed=i, model_path=get_test_model("splendor"), simulations=10,
                max_game_plies=30,
            )
            ep2 = dinoboard_engine.run_selfplay_episode(
                game_id="splendor", seed=i + 10000, model_path=get_test_model("splendor"), simulations=10,
                max_game_plies=30,
            )
            a1 = [s["action_id"] for s in ep1["samples"]]
            a2 = [s["action_id"] for s in ep2["samples"]]
            if a1 != a2:
                diff += 1
        assert diff > 10, f"only {diff}/20 splendor games differed with different seeds"

    def test_determinism_with_same_seed(self):
        """Same seed should produce identical episodes (deterministic RNG)."""
        ep1 = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=12345, model_path=get_test_model("splendor"), simulations=10,
            max_game_plies=30, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        ep2 = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=12345, model_path=get_test_model("splendor"), simulations=10,
            max_game_plies=30, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        assert ep1["total_plies"] == ep2["total_plies"]
        assert ep1["winner"] == ep2["winner"]

    def test_features_vary_across_plies(self):
        """Splendor features should change as the game progresses."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=42, model_path=get_test_model("splendor"), simulations=10,
            max_game_plies=60,
        )
        samples = ep["samples"]
        if len(samples) < 5:
            pytest.skip("too few samples")
        assert samples[0]["features"] != samples[4]["features"]

    def test_legal_actions_valid(self):
        """Splendor legal actions should be in valid range."""
        action_space = 70
        gs = dinoboard_engine.GameSession("splendor", seed=42)
        legal = gs.get_legal_actions()
        assert all(0 <= a < action_space for a in legal)

    def test_high_simulation_count_no_crash(self):
        """High sim count exercises NoPeek chance pool more heavily."""
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=42, model_path=get_test_model("splendor"), simulations=100,
            max_game_plies=30,
        )
        assert ep["total_plies"] > 0

    def test_arena_match_with_hidden_info(self):
        """Arena match for Splendor should work with hidden info."""
        m = get_test_model("splendor")
        result = dinoboard_engine.run_arena_match(
            game_id="splendor", seed=42,
            model_paths=[m, m],
            simulations_list=[10, 10], temperature=0.0,
        )
        assert "winner" in result
        assert "total_plies" in result


# ---------------------------------------------------------------------------
# Azul: bag-sampled randomness (randomize_unseen shuffles bag at root)
# ---------------------------------------------------------------------------

class TestAzulHiddenInfo:

    def test_selfplay_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=42, model_path=get_test_model("azul"), simulations=10,
            max_game_plies=100,
        )
        assert ep["total_plies"] > 0

    def test_determinism(self):
        ep1 = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=999, model_path=get_test_model("azul"), simulations=10,
            max_game_plies=50, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        ep2 = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=999, model_path=get_test_model("azul"), simulations=10,
            max_game_plies=50, dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
        )
        assert ep1["total_plies"] == ep2["total_plies"]
        assert ep1["winner"] == ep2["winner"]

    def test_features_vary(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=42, model_path=get_test_model("azul"), simulations=10,
            max_game_plies=100,
        )
        if len(ep["samples"]) < 5:
            pytest.skip("too few samples")
        assert ep["samples"][0]["features"] != ep["samples"][4]["features"]

    def test_3p_game_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul_3p", seed=42, model_path=get_test_model("azul_3p"), simulations=5,
            max_game_plies=100,
        )
        assert ep["total_plies"] > 0

    def test_4p_game_completes(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul_4p", seed=42, model_path=get_test_model("azul_4p"), simulations=5,
            max_game_plies=100,
        )
        assert ep["total_plies"] > 0

    def test_high_simulation_count_no_crash(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=42, model_path=get_test_model("azul"), simulations=100,
            max_game_plies=50,
        )
        assert ep["total_plies"] > 0


# ---------------------------------------------------------------------------
# Cross-game hidden-info invariants
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Belief tracker correctness: randomize_unseen must NOT peek at hidden state
# ---------------------------------------------------------------------------

class TestSplendorBeliefTracker:

    def test_randomized_decks_differ_from_original(self):
        """randomize_unseen must produce different deck compositions than the real deck.

        If the tracker were peeking at data.decks, every trial would have the
        same card set as the original (just reordered). Since the tracker uses
        全卡池 - seen_cards, purchased cards that left the real deck still
        appear in the unseen pool, making compositions differ.
        """
        r = dinoboard_engine.test_belief_tracker("splendor", seed=42, plies=20, randomize_trials=10)
        assert r["plies"] > 0
        orig = sorted(r["original_deck"])
        diffs = sum(1 for td in r["trial_decks"] if sorted(td) != orig)
        assert diffs > 0, "All trials matched original deck — tracker may be peeking"

    def test_tableau_cards_never_in_randomized_deck(self):
        """Cards visible on the tableau must never appear in randomized decks."""
        r = dinoboard_engine.test_belief_tracker("splendor", seed=123, plies=15, randomize_trials=10)
        tab = set(r["tableau_cards"])
        for i, td in enumerate(r["trial_decks"]):
            overlap = set(td) & tab
            assert not overlap, f"Trial {i}: tableau cards {overlap} found in randomized deck"

    def test_deck_sizes_preserved(self):
        """randomize_unseen must preserve deck size (public information)."""
        r = dinoboard_engine.test_belief_tracker("splendor", seed=99, plies=15, randomize_trials=10)
        orig_len = len(r["original_deck"])
        for i, td in enumerate(r["trial_decks"]):
            assert len(td) == orig_len, (
                f"Trial {i}: deck size {len(td)} != original {orig_len}"
            )

    def test_randomization_has_variance(self):
        """Multiple randomize_unseen calls should produce different orderings."""
        r = dinoboard_engine.test_belief_tracker("splendor", seed=77, plies=10, randomize_trials=10)
        unique = set()
        for td in r["trial_decks"]:
            unique.add(tuple(td))
        assert len(unique) > 1, "All trials produced identical decks — no randomization"

    def test_no_duplicate_cards_in_randomized_deck(self):
        """Each card ID should appear at most once in a randomized deck."""
        r = dinoboard_engine.test_belief_tracker("splendor", seed=55, plies=15, randomize_trials=5)
        for i, td in enumerate(r["trial_decks"]):
            assert len(td) == len(set(td)), (
                f"Trial {i}: duplicate card IDs in deck: {[x for x in td if td.count(x) > 1]}"
            )


# ---------------------------------------------------------------------------
# MCTS hidden-info handling — ISMCTS routes belief-tracker games through
# root sampling; there's no NoPeek / traversal_limiter mechanism. These tests
# just check that selfplay/arena completes on hidden-info games. The "does
# MCTS actually hide opponent info" invariant is covered by
# TestLoveLetterGuardAccuracy in tests/test_hidden_info_coup_loveletter.py.
# ---------------------------------------------------------------------------

class TestHiddenInfoSelfplay:

    def test_splendor_selfplay_runs(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="splendor", seed=42, model_path=get_test_model("splendor"),
            simulations=50, max_game_plies=40,
        )
        assert ep["total_plies"] > 0

    def test_azul_selfplay_runs(self):
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="azul", seed=42, model_path=get_test_model("azul"),
            simulations=100, max_game_plies=200,
        )
        assert ep["total_plies"] > 0

    def test_splendor_arena_runs(self):
        m = get_test_model("splendor")
        result = dinoboard_engine.run_arena_match(
            game_id="splendor", seed=42,
            model_paths=[m, m],
            simulations_list=[30, 30], temperature=0.0,
        )
        assert result["total_plies"] > 0


@pytest.mark.parametrize("game_id", ["splendor", "azul"])
def test_hidden_info_game_samples_have_features(game_id):
    """Hidden-info games must still produce valid features per sample."""
    expected_dim = GAME_CONFIGS[game_id]["feature_dim"]
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id), simulations=10,
        max_game_plies=60,
    )
    for s in ep["samples"]:
        assert len(s["features"]) == expected_dim, (
            f"{game_id} ply {s['ply']}: feature dim={len(s['features'])}, expected {expected_dim}"
        )


@pytest.mark.parametrize("game_id", ["splendor", "azul"])
def test_hidden_info_game_policy_valid(game_id):
    """Policy visits should only be on legal actions."""
    ep = dinoboard_engine.run_selfplay_episode(
        game_id=game_id, seed=42, model_path=get_test_model(game_id), simulations=10,
        max_game_plies=60,
    )
    for s in ep["samples"]:
        legal_set = {i for i, m in enumerate(s["legal_mask"]) if m > 0}
        visited = {aid for aid, v in zip(s["policy_action_ids"], s["policy_action_visits"]) if v > 0}
        illegal = visited - legal_set
        assert not illegal, (
            f"{game_id} ply {s['ply']}: visited illegal actions {illegal}"
        )
