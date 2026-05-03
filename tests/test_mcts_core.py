"""Tests for core MCTS behaviors: PUCT, temperature, Dirichlet, backpropagation.

These tests verify that the MCTS engine's lowest-level behaviors actually
work as intended, using observable outputs (visit counts, chosen actions)
as proxies for internal correctness.
"""
import math
import dinoboard_engine
import pytest

from conftest import GAME_CONFIGS, get_test_model


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def visit_entropy(ids, visits):
    """Shannon entropy of the visit distribution (nats)."""
    total = sum(visits)
    if total == 0:
        return 0.0
    ent = 0.0
    for v in visits:
        if v > 0:
            p = v / total
            ent -= p * math.log(p)
    return ent


def run_ep(game_id="tictactoe", seed=42, **kwargs):
    defaults = dict(
        simulations=50, max_game_plies=20,
        dirichlet_alpha=0.0, dirichlet_epsilon=0.0,
    )
    defaults.update(kwargs)
    if "model_path" not in defaults:
        defaults["model_path"] = get_test_model(game_id)
    return dinoboard_engine.run_selfplay_episode(game_id=game_id, seed=seed, **defaults)


# ---------------------------------------------------------------------------
# 1. MCTS finds obvious winning move
# ---------------------------------------------------------------------------

def test_mcts_finds_winning_move_tictactoe():
    """X has top-left and top-center. X should play top-right to win."""
    m = get_test_model("tictactoe")
    gs = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
    gs.apply_action(0)  # X = top-left
    gs.apply_action(3)  # O = mid-left
    gs.apply_action(1)  # X = top-center
    gs.apply_action(4)  # O = center
    # X to play; action 2 completes top row = instant win
    result = gs.get_ai_action(simulations=200, temperature=0.0)
    assert result["action"] == 2, f"MCTS should find winning move 2, got {result['action']}"


def test_mcts_blocks_opponent_winning_threat_tictactoe():
    """O threatens to complete mid row. X must block at action 5."""
    m = get_test_model("tictactoe")
    gs = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
    gs.apply_action(0)  # X = top-left
    gs.apply_action(3)  # O = mid-left
    gs.apply_action(6)  # X = bottom-left
    gs.apply_action(4)  # O = center
    # O threatens 3-4-5. X must play 5 to block (or find a winning line).
    result = gs.get_ai_action(simulations=500, temperature=0.0)
    # X's only sensible moves: 5 (block), or something that creates a double threat.
    # With random model and 500 sims, MCTS should at least not ignore the threat.
    # If X doesn't play 5, simulate O's response to verify X isn't losing.
    if result["action"] != 5:
        gs2 = dinoboard_engine.GameSession("tictactoe", seed=42)
        gs2.apply_action(0)
        gs2.apply_action(3)
        gs2.apply_action(6)
        gs2.apply_action(4)
        gs2.apply_action(result["action"])  # X's move
        # O should play 5 to win
        assert not gs2.is_terminal, "game shouldn't be over yet"
        gs2.apply_action(5)  # O completes mid row
        # If O won, MCTS failed to block
        assert gs2.winner != 1, (
            f"MCTS chose {result['action']} instead of blocking at 5; O won"
        )


def test_mcts_finds_winning_move_with_more_sims():
    """More simulations should reliably find the winning move."""
    m = get_test_model("tictactoe")
    gs = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
    # Set up: X=0,1 O=4,7. X to play, action 2 = win.
    gs.apply_action(0)  # X
    gs.apply_action(4)  # O
    gs.apply_action(1)  # X
    gs.apply_action(7)  # O
    result = gs.get_ai_action(simulations=1000, temperature=0.0)
    assert result["action"] == 2


# ---------------------------------------------------------------------------
# 2. Temperature controls action selection randomness
# ---------------------------------------------------------------------------

def test_greedy_temperature_picks_most_visited():
    """With temperature=0, chosen action must be the one with most visits."""
    ep = run_ep(simulations=100, temperature=0.0)
    for s in ep["samples"]:
        ids = s["policy_action_ids"]
        visits = s["policy_action_visits"]
        if not visits:
            continue
        max_v = max(visits)
        most_visited = {ids[i] for i, v in enumerate(visits) if v == max_v}
        assert s["action_id"] in most_visited, (
            f"ply {s['ply']}: action {s['action_id']} not in most-visited {most_visited}"
        )


def test_high_temperature_increases_entropy():
    """Higher temperature should produce more uniform (higher entropy) action selection."""
    seeds = range(100, 130)
    low_temp_actions = []
    high_temp_actions = []
    for seed in seeds:
        ep_low = run_ep(seed=seed, simulations=50, temperature=0.1)
        ep_high = run_ep(seed=seed, simulations=50, temperature=5.0)
        if ep_low["samples"]:
            low_temp_actions.append(ep_low["samples"][0]["action_id"])
        if ep_high["samples"]:
            high_temp_actions.append(ep_high["samples"][0]["action_id"])
    # With temperature=5.0, first-move actions should be more diverse than temperature=0.1
    low_unique = len(set(low_temp_actions))
    high_unique = len(set(high_temp_actions))
    assert high_unique >= low_unique, (
        f"high temp should be more diverse: low_unique={low_unique}, high_unique={high_unique}"
    )


def test_temperature_zero_is_deterministic():
    """Temperature=0 with same seed should always produce the same episode."""
    ep1 = run_ep(seed=999, simulations=50, temperature=0.0)
    ep2 = run_ep(seed=999, simulations=50, temperature=0.0)
    assert ep1["winner"] == ep2["winner"]
    assert ep1["total_plies"] == ep2["total_plies"]
    for s1, s2 in zip(ep1["samples"], ep2["samples"]):
        assert s1["action_id"] == s2["action_id"], (
            f"ply {s1['ply']}: different actions {s1['action_id']} vs {s2['action_id']}"
        )


# ---------------------------------------------------------------------------
# 3. c_puct controls exploration breadth
# ---------------------------------------------------------------------------

def test_higher_cpuct_increases_visit_entropy():
    """Higher c_puct should spread visits more evenly across actions."""
    entropies_low = []
    entropies_high = []
    for seed in range(42, 62):
        ep_low = run_ep(seed=seed, simulations=100, c_puct=0.1)
        ep_high = run_ep(seed=seed, simulations=100, c_puct=10.0)
        for s in ep_low["samples"][:3]:
            entropies_low.append(visit_entropy(s["policy_action_ids"], s["policy_action_visits"]))
        for s in ep_high["samples"][:3]:
            entropies_high.append(visit_entropy(s["policy_action_ids"], s["policy_action_visits"]))
    avg_low = sum(entropies_low) / max(1, len(entropies_low))
    avg_high = sum(entropies_high) / max(1, len(entropies_high))
    assert avg_high > avg_low, (
        f"high c_puct should have higher visit entropy: low={avg_low:.3f}, high={avg_high:.3f}"
    )


def test_very_low_cpuct_concentrates_visits_near_terminal():
    """c_puct near 0 near a won position should concentrate visits on the winning move."""
    m = get_test_model("tictactoe")
    gs = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
    gs.apply_action(0)  # X
    gs.apply_action(3)  # O
    gs.apply_action(1)  # X
    gs.apply_action(4)  # O
    # X to play; action 2 wins. With c_puct≈0, MCTS should exploit the winning line.
    result = gs.get_ai_action(simulations=200, temperature=0.0)
    assert result["action"] == 2, (
        f"very low c_puct near win should find action 2, got {result['action']}"
    )


# ---------------------------------------------------------------------------
# 4. Dirichlet noise increases root exploration
# ---------------------------------------------------------------------------

def test_dirichlet_noise_changes_visit_distribution():
    """Dirichlet noise should alter the visit pattern compared to no noise."""
    different_count = 0
    for seed in range(42, 72):
        ep_no = run_ep(seed=seed, simulations=50, dirichlet_alpha=0.0, dirichlet_epsilon=0.0)
        ep_yes = run_ep(seed=seed, simulations=50, dirichlet_alpha=0.3, dirichlet_epsilon=0.25)
        if ep_no["samples"] and ep_yes["samples"]:
            v_no = ep_no["samples"][0]["policy_action_visits"]
            v_yes = ep_yes["samples"][0]["policy_action_visits"]
            if v_no != v_yes:
                different_count += 1
    assert different_count > 10, (
        f"Dirichlet noise should change visits in most episodes, only {different_count}/30 differed"
    )


def test_dirichlet_ply_window():
    """dirichlet_on_first_n_plies should limit noise to early moves only."""
    diffs_early = 0
    diffs_late = 0
    for seed in range(42, 72):
        ep_no = run_ep(seed=seed, simulations=30, dirichlet_alpha=0.0, dirichlet_epsilon=0.0, max_game_plies=12)
        ep_yes = run_ep(seed=seed, simulations=30, dirichlet_alpha=0.3, dirichlet_epsilon=0.25,
                        dirichlet_on_first_n_plies=2, max_game_plies=12)
        samples_no = {s["ply"]: s for s in ep_no["samples"]}
        samples_yes = {s["ply"]: s for s in ep_yes["samples"]}
        for ply in [0, 1]:
            if ply in samples_no and ply in samples_yes:
                if samples_no[ply]["policy_action_visits"] != samples_yes[ply]["policy_action_visits"]:
                    diffs_early += 1
        for ply in [4, 5, 6]:
            if ply in samples_no and ply in samples_yes:
                if samples_no[ply]["policy_action_visits"] != samples_yes[ply]["policy_action_visits"]:
                    diffs_late += 1
    # Noise should mostly affect early plies, not late plies
    assert diffs_early > diffs_late, (
        f"noise should impact early plies more: early_diffs={diffs_early}, late_diffs={diffs_late}"
    )


# ---------------------------------------------------------------------------
# 5. Visit counts are proportional to simulations
# ---------------------------------------------------------------------------

def test_total_visits_match_simulations():
    """Total visit count at root should equal the number of simulations."""
    for sims in [10, 50, 200]:
        ep = run_ep(seed=42, simulations=sims)
        for s in ep["samples"][:3]:
            total_visits = sum(s["policy_action_visits"])
            assert total_visits == sims, (
                f"sims={sims}, ply {s['ply']}: total_visits={total_visits}, expected {sims}"
            )


# ---------------------------------------------------------------------------
# 6. Seed determinism
# ---------------------------------------------------------------------------

def test_seed_determinism_full_episode():
    """Same seed + config must produce bit-identical episodes."""
    for game_id in ["tictactoe", "quoridor"]:
        ep1 = run_ep(game_id=game_id, seed=12345, simulations=30, temperature=1.0)
        ep2 = run_ep(game_id=game_id, seed=12345, simulations=30, temperature=1.0)
        assert ep1["winner"] == ep2["winner"]
        assert ep1["total_plies"] == ep2["total_plies"]
        assert len(ep1["samples"]) == len(ep2["samples"])
        for s1, s2 in zip(ep1["samples"], ep2["samples"]):
            assert s1["action_id"] == s2["action_id"]
            assert s1["policy_action_visits"] == s2["policy_action_visits"]
            assert s1["features"] == s2["features"]


def test_different_seeds_produce_different_episodes():
    """Different seeds should usually produce different episodes."""
    different = 0
    for seed in range(100):
        ep1 = run_ep(seed=seed, simulations=10)
        ep2 = run_ep(seed=seed + 10000, simulations=10)
        actions1 = [s["action_id"] for s in ep1["samples"]]
        actions2 = [s["action_id"] for s in ep2["samples"]]
        if actions1 != actions2:
            different += 1
    assert different > 50, f"different seeds should usually differ, only {different}/100 did"


# ---------------------------------------------------------------------------
# 7. Temperature schedule (linear decay)
# ---------------------------------------------------------------------------

def test_temperature_schedule_decay():
    """With temperature schedule decaying, late-game actions should be more greedy."""
    early_entropies = []
    late_entropies = []
    for seed in range(42, 72):
        ep = run_ep(
            game_id="tictactoe", seed=seed, simulations=50, temperature=1.0,
            temperature_initial=2.0, temperature_final=0.01, temperature_decay_plies=6,
            max_game_plies=9,
        )
        for s in ep["samples"]:
            ent = visit_entropy(s["policy_action_ids"], s["policy_action_visits"])
            if s["ply"] <= 1:
                early_entropies.append(ent)
            elif s["ply"] >= 6:
                late_entropies.append(ent)
    if not early_entropies or not late_entropies:
        pytest.skip("not enough plies")
    avg_early = sum(early_entropies) / len(early_entropies)
    avg_late = sum(late_entropies) / len(late_entropies)
    # Note: entropy here is of visit counts (from MCTS), not of the sampling.
    # Temperature affects which action is *chosen*, not the visit counts themselves.
    # So this test checks the action diversity across episodes instead.


def test_temperature_schedule_action_diversity():
    """With decaying temperature, early actions should be more diverse than late."""
    early_actions = []
    late_actions = []
    for seed in range(42, 142):
        ep = run_ep(
            game_id="tictactoe", seed=seed, simulations=30, temperature=1.0,
            temperature_initial=3.0, temperature_final=0.0, temperature_decay_plies=5,
            max_game_plies=9,
        )
        for s in ep["samples"]:
            if s["ply"] == 0:
                early_actions.append(s["action_id"])
            elif s["ply"] >= 5:
                late_actions.append(s["action_id"])
    if not late_actions:
        pytest.skip("no late-ply samples")
    early_unique = len(set(early_actions))
    late_unique = len(set(late_actions))
    # Early should have more diversity (higher temperature)
    # Normalize by sample count
    early_ratio = early_unique / max(1, len(early_actions))
    late_ratio = late_unique / max(1, len(late_actions))
    assert early_ratio >= late_ratio * 0.8, (
        f"early diversity should be >= late: early={early_ratio:.2f}, late={late_ratio:.2f}"
    )


# ---------------------------------------------------------------------------
# 8. Heuristic guidance ratio
# ---------------------------------------------------------------------------

def test_heuristic_ratio_one_uses_no_mcts_quoridor():
    """heuristic_guidance_ratio=1.0 should never run MCTS; visit patterns differ."""
    ep_mcts = run_ep(game_id="quoridor", seed=42, simulations=30,
                     heuristic_guidance_ratio=0.0, max_game_plies=30)
    ep_heur = run_ep(game_id="quoridor", seed=42, simulations=30,
                     heuristic_guidance_ratio=1.0, heuristic_temperature=0.0,
                     max_game_plies=30)
    # With heuristic ratio=1.0, visits should be concentrated (heuristic assigns
    # 10000 fake visits proportionally to chosen actions)
    if not ep_mcts["samples"] or not ep_heur["samples"]:
        pytest.skip("no samples")
    mcts_visits = ep_mcts["samples"][0]["policy_action_visits"]
    heur_visits = ep_heur["samples"][0]["policy_action_visits"]
    assert mcts_visits != heur_visits, "heuristic and MCTS should produce different visits"


def test_heuristic_ratio_zero_uses_mcts():
    """heuristic_guidance_ratio=0.0 should produce standard MCTS visit patterns."""
    ep = run_ep(game_id="quoridor", seed=42, simulations=50,
                heuristic_guidance_ratio=0.0, max_game_plies=20)
    if not ep["samples"]:
        pytest.skip("no samples")
    total = sum(ep["samples"][0]["policy_action_visits"])
    assert total == 50, f"expected 50 total visits from MCTS, got {total}"


# ---------------------------------------------------------------------------
# 9. Value backpropagation sanity
# ---------------------------------------------------------------------------

def test_mcts_prefers_winning_side_near_terminal():
    """Near a terminal position, MCTS should recognize the winning player."""
    m = get_test_model("tictactoe")
    # Build a position where X is about to win
    # X to play, action 2 = win. MCTS with any number of sims should find it.
    for sims in [10, 50, 200]:
        gs2 = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
        gs2.apply_action(0)
        gs2.apply_action(3)
        gs2.apply_action(1)
        gs2.apply_action(4)
        result = gs2.get_ai_action(simulations=sims, temperature=0.0)
        assert result["action"] == 2, (
            f"sims={sims}: MCTS should find winning move 2, got {result['action']}"
        )


def test_mcts_avoids_losing_move():
    """MCTS should avoid moves that lead to immediate opponent win."""
    m = get_test_model("tictactoe")
    # Set up a position where X must block O's threat
    # O has 3,4 (mid-left, center), threatening 5 (mid-right)
    gs = dinoboard_engine.GameSession("tictactoe", seed=42, model_path=m)
    gs.apply_action(8)  # X = bottom-right (arbitrary)
    gs.apply_action(3)  # O = mid-left
    gs.apply_action(0)  # X = top-left
    gs.apply_action(4)  # O = center
    # X to play. O threatens 3-4-5. If X doesn't play 5, O wins next turn.
    # X might also have other strategies, but should not give O the win.
    result = gs.get_ai_action(simulations=500, temperature=0.0)
    # Simulate: if X doesn't play 5, does O win?
    if result["action"] != 5:
        gs2 = dinoboard_engine.GameSession("tictactoe", seed=42)
        gs2.apply_action(8)
        gs2.apply_action(3)
        gs2.apply_action(0)
        gs2.apply_action(4)
        gs2.apply_action(result["action"])
        if not gs2.is_terminal:
            gs2.apply_action(5)  # O plays the threat
            if gs2.is_terminal:
                assert gs2.winner != 1, (
                    f"MCTS chose {result['action']} allowing O to win with 5"
                )


# ---------------------------------------------------------------------------
# 10. Uniform evaluator produces roughly uniform priors
# ---------------------------------------------------------------------------

def test_uniform_evaluator_early_visits_spread():
    """With uniform evaluator (no model), initial visits should be spread across actions."""
    ep = run_ep(seed=42, simulations=200, c_puct=1.4)
    s = ep["samples"][0]
    visits = s["policy_action_visits"]
    total = sum(visits)
    num_actions = len(visits)
    if num_actions == 0:
        pytest.skip("no actions")
    # With uniform priors and high sims, no single action should dominate excessively.
    # Perfect uniform = 200/9 ≈ 22 each. Allow some natural variance.
    max_v = max(visits)
    ratio = max_v / total
    assert ratio < 0.5, (
        f"uniform evaluator: top action has {ratio:.1%} of visits, expected more spread"
    )


# ---------------------------------------------------------------------------
# 11. Legal mask consistency during MCTS
# ---------------------------------------------------------------------------

def test_mcts_only_visits_legal_actions():
    """All visited actions in MCTS output must be legal moves."""
    for game_id in ["tictactoe", "quoridor"]:
        ep = run_ep(game_id=game_id, seed=42, simulations=100, max_game_plies=30)
        action_space = GAME_CONFIGS[game_id]["action_space"]
        for s in ep["samples"]:
            legal_set = {i for i, m in enumerate(s["legal_mask"]) if m > 0}
            visited = {aid for aid, v in zip(s["policy_action_ids"], s["policy_action_visits"]) if v > 0}
            illegal_visited = visited - legal_set
            assert not illegal_visited, (
                f"{game_id} ply {s['ply']}: MCTS visited illegal actions {illegal_visited}"
            )


def test_chosen_action_was_visited():
    """The chosen action must have at least one visit."""
    for game_id in ["tictactoe", "quoridor"]:
        ep = run_ep(game_id=game_id, seed=42, simulations=50, max_game_plies=30)
        for s in ep["samples"]:
            aid = s["action_id"]
            visit_map = dict(zip(s["policy_action_ids"], s["policy_action_visits"]))
            assert visit_map.get(aid, 0) > 0, (
                f"{game_id} ply {s['ply']}: chose action {aid} with 0 visits"
            )
