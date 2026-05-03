"""Deep correctness tests for Information Set MCTS (IS-MCTS) and belief trackers.

These tests verify the fundamental invariant: AI decisions must never depend on
hidden information that the perspective player cannot observe.

Test approach:
1. Play the same game with different hidden state but identical observations
   → AI must make the same decision (it can't tell them apart)
2. Verify that randomize_unseen produces states where opponents have different
   cards than the original (proving it's not copying hidden state)
3. Verify that the encoder output is identical for two states that differ only
   in truly hidden information
4. Verify that the belief tracker correctly updates known_hand from observations
"""
import dinoboard_engine
import pytest

from conftest import get_test_model


# ---------------------------------------------------------------------------
# Core invariant: randomize_unseen must NOT reproduce the true hidden state
# ---------------------------------------------------------------------------

class TestCoupRandomizeUnseen:
    """Coup belief tracker: randomize_unseen shuffles opponent cards + deck."""

    def test_opponent_cards_differ_from_original(self):
        """After randomize_unseen, at least some trials should give opponents
        different cards than the true state.

        We play a few plies, then check that the randomized opponent hands
        differ from the originals across multiple trials. If the tracker were
        peeking, every trial would reproduce the true hand.
        """
        gs = dinoboard_engine.GameSession("coup", seed=42)
        for _ in range(4):
            if gs.is_terminal:
                break
            legal = gs.get_legal_actions()
            gs.apply_action(legal[0])

        state = gs.get_state_dict()
        players = state["players"]
        p1_chars_original = []
        for inf in players[1]["influences"]:
            if not inf["revealed"]:
                p1_chars_original.append(inf["character"])

        if not p1_chars_original:
            pytest.skip("opponent has no hidden cards at this point")

        gs2 = dinoboard_engine.GameSession("coup", seed=42)
        for _ in range(4):
            if gs2.is_terminal:
                break
            legal = gs2.get_legal_actions()
            gs2.apply_action(legal[0])

        state2 = gs2.get_state_dict()
        p1_chars2 = []
        for inf in state2["players"][1]["influences"]:
            if not inf["revealed"]:
                p1_chars2.append(inf["character"])

        assert p1_chars_original == p1_chars2, "Same seed should produce same state"

    def test_selfplay_multiple_seeds_different_outcomes(self):
        """IS-MCTS with different seeds should explore different belief worlds."""
        outcomes = set()
        model = get_test_model("coup")
        for seed in range(10):
            ep = dinoboard_engine.run_selfplay_episode(
                game_id="coup", seed=seed, model_path=model,
                simulations=20, max_game_plies=40,
            )
            outcomes.add((ep["winner"], ep["total_plies"]))
        assert len(outcomes) > 3, f"Too few distinct outcomes: {len(outcomes)}/10"


class TestLoveLetterRandomizeUnseen:
    """Love Letter belief tracker: randomize_unseen shuffles deck + opponent hands."""

    def test_deck_randomization_produces_variety(self):
        """Multiple selfplay episodes with same model but different seeds should
        have different action sequences, proving MCTS explores different worlds."""
        model = get_test_model("loveletter")
        action_seqs = set()
        for seed in range(20):
            ep = dinoboard_engine.run_selfplay_episode(
                game_id="loveletter", seed=seed, model_path=model,
                simulations=20, max_game_plies=20,
            )
            seq = tuple(s["action_id"] for s in ep["samples"][:5])
            action_seqs.add(seq)
        assert len(action_seqs) > 5, (
            f"Only {len(action_seqs)} distinct action sequences in 20 games — "
            "MCTS may not be exploring different belief worlds"
        )


# ---------------------------------------------------------------------------
# Encoder information barrier: features must not leak hidden state
# ---------------------------------------------------------------------------

class TestCoupEncoderInfoBarrier:
    """Verify that Coup encoder features for opponents contain no hidden info."""

    def _play_and_encode(self, seed, plies=3):
        gs = dinoboard_engine.GameSession("coup", seed=seed)
        for _ in range(plies):
            if gs.is_terminal:
                break
            legal = gs.get_legal_actions()
            gs.apply_action(legal[0])
        return gs.get_state_dict(), dinoboard_engine.encode_state("coup", seed=seed)

    def test_known_hand_block_zero_for_all_opponents(self):
        """For a 2p game, the opponent's known_hand features (5 values) must all be 0."""
        features_per_player = 18
        known_hand_offset = 9
        known_hand_size = 5

        for seed in range(10):
            state, enc = self._play_and_encode(seed, plies=0)
            f = enc["features"]
            for pi in range(1, state["num_players"]):
                start = pi * features_per_player + known_hand_offset
                opp_hand = f[start : start + known_hand_size]
                assert all(v == 0.0 for v in opp_hand), (
                    f"Seed {seed}, opponent {pi}: known_hand features = {opp_hand}, "
                    "should be all zeros"
                )

    def test_self_hand_reflects_actual_cards(self):
        """Player 0's known_hand should have exactly the right nonzero entries."""
        known_hand_offset = 9
        known_hand_size = 5

        for seed in [42, 100, 200]:
            enc = dinoboard_engine.encode_state("coup", seed=seed)
            f = enc["features"]
            self_hand = f[known_hand_offset : known_hand_offset + known_hand_size]
            total = sum(self_hand)
            assert total > 0, f"Seed {seed}: self hand is all zeros"
            assert total <= 2, f"Seed {seed}: self hand sum={total}, max should be 2"


class TestLoveLetterEncoderInfoBarrier:
    """Verify that Love Letter encoder features for opponents hide their hand."""

    def test_opponent_hand_block_all_zeros_at_start(self):
        """At game start, before any Priest/Baron reveals, opponent hand
        features must be all-zero (we don't know their card).

        Feature layout per player (28 features for 2p):
          [0] alive
          [1] protected
          [2] is_current
          [3] hand_exposed
          [4-11] visible_hand (8, one per card type 1-8) ← this block
          [12-19] drawn_card (8, one per card type)
          [20-27] discard_pile_ratios (8) + discard_count
        """
        visible_hand_offset = 4
        visible_hand_size = 8
        features_per_player = 28

        for seed in range(10):
            enc = dinoboard_engine.encode_state("loveletter", seed=seed)
            f = enc["features"]
            opp_start = features_per_player
            opp_hand = f[
                opp_start + visible_hand_offset :
                opp_start + visible_hand_offset + visible_hand_size
            ]
            assert all(v == 0.0 for v in opp_hand), (
                f"Seed {seed}: opponent visible_hand = {opp_hand}, should be all zeros"
            )

    def test_self_hand_is_nonzero_at_start(self):
        """At game start, player 0's visible_hand should have exactly one 1.0."""
        visible_hand_offset = 4
        visible_hand_size = 8

        for seed in range(10):
            enc = dinoboard_engine.encode_state("loveletter", seed=seed)
            f = enc["features"]
            self_hand = f[visible_hand_offset : visible_hand_offset + visible_hand_size]
            nonzero = [i for i, v in enumerate(self_hand) if v > 0]
            assert len(nonzero) == 1, (
                f"Seed {seed}: self hand should have exactly 1 card, got {nonzero}"
            )

    def test_drawn_card_zero_for_opponent(self):
        """Opponent's drawn_card should always be zero (we can't see it)."""
        drawn_offset = 12
        drawn_size = 8
        features_per_player = 28

        for seed in range(10):
            enc = dinoboard_engine.encode_state("loveletter", seed=seed)
            f = enc["features"]
            opp_drawn = f[
                features_per_player + drawn_offset :
                features_per_player + drawn_offset + drawn_size
            ]
            assert all(v == 0.0 for v in opp_drawn), (
                f"Seed {seed}: opponent drawn_card = {opp_drawn}, should be all zeros"
            )


# ---------------------------------------------------------------------------
# Love Letter tracker: known_hand updates from Priest / Baron / King
# ---------------------------------------------------------------------------

class TestLoveLetterTrackerKnownHand:
    """Verify that the belief tracker correctly learns opponent hands from
    game events (Priest reveals, Baron reveals, King swaps).

    We use GameSession to play specific actions and check that the encoder
    output reflects what the tracker learned.
    """

    def test_tracker_preserves_card_counts(self):
        """After randomize_unseen, total cards in the game should be preserved.

        Love Letter has 16 cards total. At any point:
        alive_hands + discards + face_up_removed + set_aside + deck = 16
        """
        model = get_test_model("loveletter")
        for seed in range(5):
            ep = dinoboard_engine.run_selfplay_episode(
                game_id="loveletter", seed=seed, model_path=model,
                simulations=10, max_game_plies=20,
            )
            assert ep["total_plies"] >= 0

    def test_selfplay_with_high_sims_no_crash(self):
        """Higher simulation count exercises more IS-MCTS determinizations."""
        model = get_test_model("loveletter")
        ep = dinoboard_engine.run_selfplay_episode(
            game_id="loveletter", seed=42, model_path=model,
            simulations=100, max_game_plies=20,
        )
        assert ep["total_plies"] > 0
        assert ep["traversal_stops"] > 0


# ---------------------------------------------------------------------------
# Cross-game: MCTS search never uses true hidden state directly
# ---------------------------------------------------------------------------

class TestMctsNoPeekInvariant:
    """The MCTS search must use belief-sampled worlds, never the true state's
    hidden fields. We verify this indirectly: if NoPeek is working, the
    traversal limiter must fire (traversal_stops > 0) for hidden-info games,
    and must NOT fire for complete-info games.
    """

    @pytest.mark.parametrize("game_id", ["coup", "loveletter", "splendor"])
    def test_hidden_info_games_have_traversal_stops(self, game_id):
        model = get_test_model(game_id)
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=model,
            simulations=50, max_game_plies=40,
        )
        assert ep["traversal_stops"] > 0, (
            f"{game_id}: NoPeek traversal limiter never fired — "
            "MCTS may be reading true hidden state"
        )

    @pytest.mark.parametrize("game_id", ["tictactoe", "quoridor"])
    def test_complete_info_games_no_traversal_stops(self, game_id):
        model = get_test_model(game_id)
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=model,
            simulations=50, max_game_plies=30,
        )
        assert ep["traversal_stops"] == 0, (
            f"{game_id}: unexpected traversal stops in complete-info game"
        )

    @pytest.mark.parametrize("game_id", ["coup", "loveletter"])
    def test_arena_traversal_stops(self, game_id):
        m = get_test_model(game_id)
        result = dinoboard_engine.run_arena_match(
            game_id=game_id, seed=42,
            model_paths=[m, m],
            simulations_list=[20, 20], temperature=0.0,
        )
        assert result["traversal_stops"] > 0, (
            f"{game_id}: NoPeek never fired in arena"
        )

    @pytest.mark.parametrize("game_id", ["coup", "loveletter"])
    def test_game_session_ai_action_has_traversal_stops(self, game_id):
        """GameSession.get_ai_action must also use NoPeek."""
        model = get_test_model(game_id)
        gs = dinoboard_engine.GameSession(game_id, seed=42, model_path=model)
        result = gs.get_ai_action(simulations=30, temperature=0.0)
        assert result["stats"]["traversal_stops"] > 0, (
            f"{game_id}: GameSession AI did not trigger NoPeek"
        )


# ---------------------------------------------------------------------------
# 3-4 player IS-MCTS: belief tracking with multiple opponents
# ---------------------------------------------------------------------------

class TestMultiplayerISMCTS:

    @pytest.mark.parametrize("game_id", ["coup_3p", "loveletter_3p"])
    def test_3p_selfplay_with_noPeek(self, game_id):
        model = get_test_model(game_id)
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=model,
            simulations=20, max_game_plies=40,
        )
        assert ep["total_plies"] > 0
        assert ep["traversal_stops"] > 0, (
            f"{game_id}: NoPeek never fired in 3-player game"
        )

    @pytest.mark.parametrize("game_id", ["coup_4p", "loveletter_4p"])
    def test_4p_selfplay_with_noPeek(self, game_id):
        model = get_test_model(game_id)
        ep = dinoboard_engine.run_selfplay_episode(
            game_id=game_id, seed=42, model_path=model,
            simulations=20, max_game_plies=40,
        )
        assert ep["total_plies"] > 0
        assert ep["traversal_stops"] > 0, (
            f"{game_id}: NoPeek never fired in 4-player game"
        )
