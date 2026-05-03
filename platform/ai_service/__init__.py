"""AI inference API service.

This service exposes the AI as a session-based API. It is designed so that a
digital board game server can outsource AI decisions to us by sending ONLY
observations (action IDs + public events) and receiving ONLY action IDs back.

The API contract never accepts or returns a game state object. Any code
reviewer can verify the separation-of-concerns principle (CLAUDE.md: "AI
pipeline must work solely from observation history") by auditing the four
endpoints: no state crosses the boundary.

Limitations of the MVP:
- Fully deterministic games (TicTacToe, Quoridor) work as-is.
- Stochastic games (Splendor, Love Letter, Coup, Azul) currently require
  the API and ground truth to share a seed for internal hidden state to
  align. A future v2 will replace this with a per-game public-event
  protocol so the API can maintain its own belief independently.
"""
