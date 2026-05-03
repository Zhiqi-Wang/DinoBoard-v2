"""AI API routes. Contract: no game state crosses this boundary.

Endpoints:
  POST /ai/sessions              — create a session, returns session_id
  POST /ai/sessions/{id}/observe — record an action the AI observed
  POST /ai/sessions/{id}/decide  — ask the AI for its next action
  DELETE /ai/sessions/{id}       — close a session
  GET /ai/sessions/{id}          — status (actions observed, turn, terminal)
"""
from __future__ import annotations

from typing import Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from .sessions import get_store

router = APIRouter(prefix="/ai/sessions", tags=["ai"])


class CreateSessionRequest(BaseModel):
    game_id: str = Field(..., description="e.g. 'quoridor', 'splendor'")
    seed: int = Field(..., description="RNG seed for internal belief/state construction. "
                                       "For stochastic games in MVP, must match ground truth.")
    my_seat: int = Field(..., description="Which player the AI is playing as (0-indexed)")
    simulations: int = Field(800, description="MCTS simulations per decision")
    temperature: float = Field(0.0, description="Action selection temperature. 0 = greedy.")


class CreateSessionResponse(BaseModel):
    session_id: str
    game_id: str
    num_players: int
    my_seat: int
    current_player: int
    is_terminal: bool


class ObserveRequest(BaseModel):
    action_id: int = Field(..., description="The action that was just played by the "
                                            "current player (NOT the AI's own moves — "
                                            "those come from /decide).")


class ObserveResponse(BaseModel):
    actions_observed: int
    current_player: Optional[int]
    is_terminal: bool


class DecideResponse(BaseModel):
    action_id: int
    action_info: dict
    stats: dict
    current_player: Optional[int]
    is_terminal: bool


class StatusResponse(BaseModel):
    session_id: str
    closed: bool
    game_id: Optional[str] = None
    num_players: Optional[int] = None
    my_seat: Optional[int] = None
    is_terminal: Optional[bool]
    current_player: Optional[int]
    winner: Optional[int] = None
    actions_observed: int


@router.post("", response_model=CreateSessionResponse)
def create_session(req: CreateSessionRequest):
    try:
        sess = get_store().create(
            game_id=req.game_id,
            seed=req.seed,
            my_seat=req.my_seat,
            simulations=req.simulations,
            temperature=req.temperature,
        )
    except (ValueError, FileNotFoundError) as e:
        raise HTTPException(400, str(e))

    st = sess.status()
    return CreateSessionResponse(
        session_id=sess.session_id,
        game_id=sess.game_id,
        num_players=sess.num_players,
        my_seat=sess.my_seat,
        current_player=st["current_player"] if st["current_player"] is not None else -1,
        is_terminal=st["is_terminal"] if st["is_terminal"] is not None else False,
    )


@router.post("/{session_id}/observe", response_model=ObserveResponse)
def observe(session_id: str, req: ObserveRequest):
    try:
        sess = get_store().get(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))

    try:
        sess.observe(req.action_id)
    except ValueError as e:
        raise HTTPException(400, str(e))
    except RuntimeError as e:
        raise HTTPException(409, str(e))

    st = sess.status()
    return ObserveResponse(
        actions_observed=st["actions_observed"],
        current_player=st["current_player"],
        is_terminal=st["is_terminal"] or False,
    )


@router.post("/{session_id}/decide", response_model=DecideResponse)
def decide(session_id: str):
    try:
        sess = get_store().get(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))

    try:
        result = sess.decide()
    except RuntimeError as e:
        raise HTTPException(409, str(e))

    st = sess.status()
    return DecideResponse(
        action_id=result["action_id"],
        action_info=result["action_info"],
        stats=result["stats"],
        current_player=st["current_player"],
        is_terminal=st["is_terminal"] or False,
    )


@router.get("/{session_id}", response_model=StatusResponse)
def get_status(session_id: str):
    try:
        sess = get_store().get(session_id)
    except KeyError as e:
        raise HTTPException(404, str(e))
    return StatusResponse(**sess.status())


@router.delete("/{session_id}")
def close_session(session_id: str):
    get_store().close(session_id)
    return {"closed": True}
