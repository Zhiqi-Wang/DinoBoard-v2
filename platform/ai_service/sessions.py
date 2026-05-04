"""AI-only session: observation-in, action-out. No state crosses the API boundary."""
from __future__ import annotations

import threading
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import dinoboard_engine as engine

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


def _base_game_id(game_id: str) -> str:
    import re
    return re.sub(r"_\d+p$", "", game_id)


def _find_model_path(game_id: str) -> str:
    """Resolve the deployed model for a game.

    Convention: games/<base>/model/<variant>.onnx. All variants of a game
    share one model/ directory. The base 2p id is expanded to '<game>_2p'.
    Same resolver as `platform/game_service/sessions.py::find_model_path`.
    """
    base = _base_game_id(game_id)
    model_dir = _PROJECT_ROOT / "games" / base / "model"
    variant_name = game_id if game_id != base else f"{base}_2p"
    candidate = model_dir / f"{variant_name}.onnx"
    if candidate.exists():
        return str(candidate)
    alt = model_dir / f"{game_id}.onnx"
    if alt.exists():
        return str(alt)
    return ""


@dataclass
class AISession:
    """Observation-only AI session.

    Internally wraps a C++ GameSession; the internal GameSession is an
    implementation detail and is never exposed to callers. The API surface
    accepts only (game_id, seed, my_seat) at creation and action IDs as
    observations, returning only action IDs on decide.

    Thread-safety: each method acquires the session's lock; concurrent calls
    to the same session serialize. Different sessions are independent.
    """

    session_id: str
    game_id: str
    num_players: int
    my_seat: int
    simulations: int
    temperature: float

    _gs: engine.GameSession = field(repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)
    _action_count: int = 0
    _closed: bool = False

    def observe(self, action_id: int) -> None:
        """Record that a player played action_id.

        Caller must send observations in turn order. The AI's own moves are
        applied via decide() and MUST NOT be re-sent via observe(). This
        mirrors how an external game server would notify the AI of OTHER
        players' moves between its own decisions.
        """
        with self._lock:
            if self._closed:
                raise RuntimeError(f"AISession {self.session_id} is closed")
            if self._gs.is_terminal:
                raise RuntimeError(
                    f"AISession {self.session_id}: game is already terminal, "
                    f"cannot observe more actions")
            legal = self._gs.get_legal_actions()
            if action_id not in legal:
                raise ValueError(
                    f"AISession {self.session_id}: observed action {action_id} "
                    f"is not legal from current position. Legal: {legal}. "
                    f"The ground truth and AI may have diverged — for stochastic "
                    f"games, ensure the AI session is seeded consistently with "
                    f"ground truth (MVP limitation; future v2 will use a "
                    f"public-event protocol instead).")
            self._gs.apply_action(action_id)
            self._action_count += 1

    def decide(self) -> dict:
        """Ask the AI to pick an action at the current position.

        Returns {"action_id": int, "action_info": dict, "stats": dict}. The
        caller is expected to apply this action to its ground-truth game and
        then send observations for any subsequent non-AI moves.

        The returned action has already been committed to this session's
        internal state; the caller must NOT re-send it via observe().
        """
        with self._lock:
            if self._closed:
                raise RuntimeError(f"AISession {self.session_id} is closed")
            if self._gs.is_terminal:
                raise RuntimeError(
                    f"AISession {self.session_id}: game is already terminal")
            current = self._gs.current_player
            if current != self.my_seat:
                raise RuntimeError(
                    f"AISession {self.session_id}: it is player {current}'s "
                    f"turn, but this session is configured for seat "
                    f"{self.my_seat}. The caller must have missed an observe()."
                )
            result = self._gs.get_ai_action(self.simulations, self.temperature)
            action_id = result["action"]
            self._gs.apply_action(action_id)
            self._action_count += 1
            return {
                "action_id": action_id,
                "action_info": result["action_info"],
                "stats": result.get("stats", {}),
            }

    def status(self) -> dict:
        """Return lightweight status. Never returns game-state fields."""
        with self._lock:
            if self._closed:
                return {
                    "session_id": self.session_id,
                    "closed": True,
                    "is_terminal": None,
                    "current_player": None,
                    "winner": None,
                    "actions_observed": self._action_count,
                }
            return {
                "session_id": self.session_id,
                "closed": False,
                "game_id": self.game_id,
                "num_players": self.num_players,
                "my_seat": self.my_seat,
                "is_terminal": self._gs.is_terminal,
                "current_player": self._gs.current_player if not self._gs.is_terminal else None,
                "winner": self._gs.winner if self._gs.is_terminal else None,
                "actions_observed": self._action_count,
            }

    def close(self) -> None:
        with self._lock:
            self._closed = True


class SessionStore:
    """In-memory session registry. FastAPI instantiates one per process."""

    def __init__(self) -> None:
        self._sessions: dict[str, AISession] = {}
        self._lock = threading.Lock()

    def create(
        self,
        game_id: str,
        seed: int,
        my_seat: int,
        simulations: int = 800,
        temperature: float = 0.0,
        model_path_override: Optional[str] = None,
    ) -> AISession:
        if game_id not in engine.available_games():
            raise ValueError(
                f"unknown game_id {game_id!r}. Available: {engine.available_games()}")

        meta = engine.game_metadata(game_id)
        num_players = meta["num_players"]
        if my_seat < 0 or my_seat >= num_players:
            raise ValueError(
                f"my_seat={my_seat} out of range for game with {num_players} players")

        if model_path_override is not None:
            model_path = model_path_override
        else:
            model_path = _find_model_path(game_id)
        if not model_path:
            base = _base_game_id(game_id)
            variant = game_id if game_id != base else f"{base}_2p"
            raise FileNotFoundError(
                f"no trained model found for {game_id}. "
                f"Expected at games/{base}/model/{variant}.onnx.")

        gs = engine.GameSession(game_id, seed, model_path, False)

        session_id = uuid.uuid4().hex[:12]
        sess = AISession(
            session_id=session_id,
            game_id=game_id,
            num_players=num_players,
            my_seat=my_seat,
            simulations=simulations,
            temperature=temperature,
            _gs=gs,
        )
        with self._lock:
            self._sessions[session_id] = sess
        return sess

    def get(self, session_id: str) -> AISession:
        with self._lock:
            sess = self._sessions.get(session_id)
        if sess is None:
            raise KeyError(f"session {session_id} not found")
        return sess

    def close(self, session_id: str) -> None:
        with self._lock:
            sess = self._sessions.pop(session_id, None)
        if sess is not None:
            sess.close()

    def count(self) -> int:
        with self._lock:
            return len(self._sessions)


# Process-global store. The platform app creates one and passes it to routes.
_STORE: Optional[SessionStore] = None


def get_store() -> SessionStore:
    global _STORE
    if _STORE is None:
        _STORE = SessionStore()
    return _STORE
