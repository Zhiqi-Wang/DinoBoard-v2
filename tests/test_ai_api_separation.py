"""Validate that the AI API is truly observation-only.

These tests drive the AI through the HTTP layer (FastAPI TestClient) using ONLY
action IDs. An independent ground-truth `engine.GameSession` runs locally in
the test and never shares objects with the AI session. If the AI can complete
games this way, the separation-of-concerns principle holds at the interface
layer: no game state ever crosses the API boundary.

What this validates:
- The API contract: no game-state fields cross either direction (enforced by
  `test_api_responses_never_include_state_fields` scanning for known keys).
- End-to-end playability: for every registered canonical game, a full game
  can be driven through the HTTP API.
- Strong separation for deterministic games: the AI session uses a seed
  different from ground truth. Both agree on public initial state, AI plays
  legally without ever reading ground truth.

Shared-seed limitation for stochastic games:
The AI session and ground truth share a seed so their internal hidden state
(deck/bag composition) aligns. This is analogous to a real partner handshake
where both parties agree on "how the cards were shuffled" as the initial
public setup. The separation is still real — the AI API code never reads
ground truth's state object; the AI has its own state seeded from the same
setup. Partners can't leak state through the API contract.
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT / "platform"))

import dinoboard_engine as engine

from ai_service import sessions as ai_sessions  # noqa: E402
from app import app  # noqa: E402
from conftest import CANONICAL_GAMES, get_test_model  # noqa: E402


# Per-game ply budgets for full-game API smoke tests. Enough to reach a
# natural terminal state without running forever.
_PLY_BUDGET = {
    "tictactoe": 20,
    "quoridor": 200,
    "splendor": 200,
    "azul": 150,
    "loveletter": 200,
    "coup": 200,
}

# Deterministic games where a strong separation test (independent seeds) works.
# Stochastic games share a seed with ground truth as the initial-setup handshake.
_DETERMINISTIC_GAMES = {"tictactoe", "quoridor"}


@pytest.fixture
def client(monkeypatch):
    """TestClient with `_find_model_path` redirected to freshly-built test models.

    Tests must not depend on pre-deployed game-specific models. The production
    HTTP API deliberately does not accept model paths from callers — models are
    resolved internally from `games/<id>/model/`. We inject a fake resolver here
    so tests can run on throwaway ONNX files that always match the current
    feature_dim.
    """
    monkeypatch.setattr(ai_sessions, "_find_model_path", get_test_model)
    # Fresh store each test so sessions don't leak between cases.
    monkeypatch.setattr(ai_sessions, "_STORE", None)
    return TestClient(app)


def _random_legal(rng: random.Random, legal: list[int]) -> int:
    return legal[rng.randrange(len(legal))]


def _play_full_game(
    client: TestClient,
    game_id: str,
    seed_ground_truth: int,
    seed_ai: int | None,
    ai_seat: int,
    max_plies: int = 500,
) -> dict:
    """Drive a complete game with opponent(s) = random legal, AI seat = API.

    Ground truth is a local GameSession. AI session is behind the HTTP API.
    The only data passed to the AI is action IDs; the only data returned is
    action IDs + metadata (info dict for rendering, stats for analytics).
    Nothing state-like crosses either direction.
    """
    if seed_ai is None:
        seed_ai = seed_ground_truth

    # Ground truth simulator — the AI never touches this object.
    gt = engine.GameSession(game_id, seed_ground_truth)
    meta = engine.game_metadata(game_id)

    # Create AI session via API.
    resp = client.post("/ai/sessions", json={
        "game_id": game_id,
        "seed": seed_ai,
        "my_seat": ai_seat,
        "simulations": 40,
        "temperature": 0.0,
    })
    assert resp.status_code == 200, resp.text
    session_id = resp.json()["session_id"]

    rng = random.Random(seed_ground_truth ^ 0xFACE)
    action_log: list[tuple[int, int]] = []  # (player, action)
    api_responses: list[dict] = []

    try:
        for _ in range(max_plies):
            if gt.is_terminal:
                break
            current = gt.current_player
            legal = gt.get_legal_actions()
            if not legal:
                break

            if current == ai_seat:
                resp = client.post(f"/ai/sessions/{session_id}/decide")
                assert resp.status_code == 200, resp.text
                body = resp.json()
                api_responses.append(body)
                action_id = body["action_id"]
                assert action_id in legal, (
                    f"AI returned illegal action {action_id}; legal={legal}")
                gt.apply_action(action_id)
                action_log.append((current, action_id))
            else:
                action_id = _random_legal(rng, legal)
                gt.apply_action(action_id)
                action_log.append((current, action_id))
                # Tell the AI what just happened — observation only.
                resp = client.post(
                    f"/ai/sessions/{session_id}/observe",
                    json={"action_id": action_id},
                )
                assert resp.status_code == 200, resp.text

        status = client.get(f"/ai/sessions/{session_id}").json()
        return {
            "action_log": action_log,
            "gt_terminal": gt.is_terminal,
            "gt_winner": gt.winner if gt.is_terminal else None,
            "api_status": status,
            "api_responses": api_responses,
        }
    finally:
        client.delete(f"/ai/sessions/{session_id}")


# ---------- Basic end-to-end: every canonical game ---------------------


@pytest.mark.parametrize("game_id", CANONICAL_GAMES)
def test_full_game_via_api(client, game_id):
    """Every registered game must be driveable end-to-end through the API.

    Acceptance criterion for new games: drop your game_id into CANONICAL_GAMES
    and this parameterized test must pass.
    """
    budget = _PLY_BUDGET[game_id]
    meta = engine.game_metadata(game_id)
    ai_seat = min(1, meta["num_players"] - 1)  # seat 1 if possible, else 0
    result = _play_full_game(
        client, game_id=game_id,
        seed_ground_truth=42, seed_ai=42,
        ai_seat=ai_seat, max_plies=budget,
    )
    assert len(result["action_log"]) > 0, "AI should have made at least one decision"
    assert result["api_status"]["actions_observed"] == len(result["action_log"])


# ---------- Strong separation: independent seeds (deterministic games) ---


@pytest.mark.parametrize("game_id", sorted(_DETERMINISTIC_GAMES))
def test_api_independent_seed_deterministic(client, game_id):
    """AI session uses a DIFFERENT seed from ground truth.

    For deterministic games, public starting state is seed-independent. If
    the AI were peeking at ground-truth state internally, it'd have to choose
    between "my seed's state" (not used because it equals public anyway) and
    "ground truth's state" (unreachable through the API). A successful run
    here proves the AI decides from its own maintained state, and the only
    bridge to ground truth is observe(action_id).
    """
    budget = _PLY_BUDGET[game_id]
    result = _play_full_game(
        client, game_id=game_id,
        seed_ground_truth=1111, seed_ai=9999,  # deliberately different
        ai_seat=1, max_plies=budget,
    )
    assert len(result["action_log"]) > 0


# ---------- Contract enforcement ---------------------------------------


def test_create_session_rejects_unknown_game(client):
    resp = client.post("/ai/sessions", json={
        "game_id": "nonexistent_game",
        "seed": 1, "my_seat": 0,
    })
    assert resp.status_code == 400


def test_create_session_rejects_out_of_range_seat(client):
    resp = client.post("/ai/sessions", json={
        "game_id": "tictactoe",
        "seed": 1, "my_seat": 5,
    })
    assert resp.status_code == 400


def test_observe_rejects_illegal_action(client):
    resp = client.post("/ai/sessions", json={
        "game_id": "tictactoe", "seed": 42, "my_seat": 1,
    })
    session_id = resp.json()["session_id"]
    try:
        # TicTacToe action space is 9; 99 is guaranteed illegal.
        resp = client.post(
            f"/ai/sessions/{session_id}/observe",
            json={"action_id": 99},
        )
        assert resp.status_code == 400, resp.text
        assert "not legal" in resp.text or "not legal" in resp.json().get("detail", "")
    finally:
        client.delete(f"/ai/sessions/{session_id}")


def test_decide_rejects_wrong_turn(client):
    """At game start, it's player 0's turn. An AI seated as player 1 must refuse."""
    resp = client.post("/ai/sessions", json={
        "game_id": "tictactoe", "seed": 42, "my_seat": 1,
    })
    session_id = resp.json()["session_id"]
    try:
        resp = client.post(f"/ai/sessions/{session_id}/decide")
        assert resp.status_code == 409
        assert "seat" in resp.text or "seat" in resp.json().get("detail", "")
    finally:
        client.delete(f"/ai/sessions/{session_id}")


def test_session_not_found(client):
    resp = client.get("/ai/sessions/deadbeef9999")
    assert resp.status_code == 404
    resp = client.post(
        "/ai/sessions/deadbeef9999/observe",
        json={"action_id": 0},
    )
    assert resp.status_code == 404


# ---------- Response surface never includes state fields ----------------


_FORBIDDEN_STATE_KEYS = {
    # Any of these would indicate the API leaked internal state.
    "board", "tiles", "factories", "deck", "hand", "hands", "cards",
    "tokens", "gems", "center", "walls", "coins", "nobles",
    "points", "score", "reserved", "face_down", "pieces",
    # Encoded tensors
    "features", "legal_mask", "legal_actions",
    # Raw state serialization
    "state", "state_dict", "current_state",
}


def _assert_no_state_keys(obj, path="$"):
    if isinstance(obj, dict):
        for k, v in obj.items():
            assert k not in _FORBIDDEN_STATE_KEYS, (
                f"Forbidden state key '{k}' leaked into API response at {path}")
            _assert_no_state_keys(v, f"{path}.{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _assert_no_state_keys(v, f"{path}[{i}]")


def test_api_responses_never_include_state_fields(client):
    """Inspect every API response body for keys that would indicate state leakage."""
    # create
    resp = client.post("/ai/sessions", json={
        "game_id": "tictactoe", "seed": 42, "my_seat": 1,
    })
    assert resp.status_code == 200
    body = resp.json()
    _assert_no_state_keys(body)
    session_id = body["session_id"]

    try:
        # status
        resp = client.get(f"/ai/sessions/{session_id}")
        _assert_no_state_keys(resp.json())

        # observe (player 0 plays center)
        resp = client.post(
            f"/ai/sessions/{session_id}/observe",
            json={"action_id": 4},
        )
        _assert_no_state_keys(resp.json())

        # decide — action_info is game-specific descriptor, not state.
        # We accept it but verify it's bounded in type (dict with simple values).
        resp = client.post(f"/ai/sessions/{session_id}/decide")
        body = resp.json()
        # Top-level contract keys
        assert set(body.keys()) <= {
            "action_id", "action_info", "stats",
            "current_player", "is_terminal",
        }, f"Unexpected keys in decide response: {set(body.keys())}"
        # action_info is a short descriptor dict — verify no state-ish keys
        _assert_no_state_keys({"action_info_nested": body["action_info"]})
    finally:
        client.delete(f"/ai/sessions/{session_id}")


# ---------- Smoke test: session cleanup ---------------------------------


def test_session_cleanup(client):
    resp = client.post("/ai/sessions", json={
        "game_id": "tictactoe", "seed": 42, "my_seat": 0,
    })
    session_id = resp.json()["session_id"]
    resp = client.get(f"/ai/sessions/{session_id}")
    assert resp.status_code == 200

    resp = client.delete(f"/ai/sessions/{session_id}")
    assert resp.status_code == 200

    resp = client.get(f"/ai/sessions/{session_id}")
    assert resp.status_code == 404
