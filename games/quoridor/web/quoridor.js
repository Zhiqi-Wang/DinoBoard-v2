import { createApp } from '/static/general/app.js';
import { apiGet, API_BASE } from '/static/general/api.js';

const BOARD_SIZE = 9;

function rcKey(r, c) { return r + ',' + c; }

function formatMove(info, actionId) {
  if (!info) {
    if (actionId !== null && actionId !== undefined) return '动作 #' + actionId;
    return '开局';
  }
  const type = String(info.type || '');
  const row = Number(info.row != null ? info.row : -1);
  const col = Number(info.col != null ? info.col : -1);
  if (type === 'move') return '走子到 (' + (row + 1) + ', ' + (col + 1) + ')';
  if (type === 'hwall') return '放横墙 @ (' + (row + 1) + ', ' + (col + 1) + ')';
  if (type === 'vwall') return '放竖墙 @ (' + (row + 1) + ', ' + (col + 1) + ')';
  if (actionId !== null && actionId !== undefined) return '动作 #' + actionId;
  return '开局';
}

function buildLegalMaps(legalActions) {
  const moves = new Map(), hwalls = new Map(), vwalls = new Map();
  if (!legalActions) return { moves, hwalls, vwalls };
  for (const actionId of legalActions) {
    if (actionId < 81) {
      moves.set(rcKey(Math.floor(actionId / 9), actionId % 9), actionId);
    } else if (actionId < 145) {
      const x = actionId - 81;
      hwalls.set(rcKey(Math.floor(x / 8), x % 8), actionId);
    } else if (actionId < 209) {
      const y = actionId - 145;
      vwalls.set(rcKey(Math.floor(y / 8), y % 8), actionId);
    }
  }
  return { moves, hwalls, vwalls };
}

function buildWallAnchorSets(st) {
  const hAnchors = new Set(), vAnchors = new Set();
  if (!st) return { hAnchors, vAnchors };
  for (const w of (st.horizontal_walls || [])) hAnchors.add(rcKey(w.row, w.col));
  for (const w of (st.vertical_walls || [])) vAnchors.add(rcKey(w.row, w.col));
  return { hAnchors, vAnchors };
}

function renderGridTo(boardEl, st, canPlay, legal, wallOwners) {
  boardEl.innerHTML = '';
  if (!st) return;

  const { hAnchors, vAnchors } = buildWallAnchorSets(st);
  const pawnByCell = new Map();
  for (const p of (st.pawns || [])) pawnByCell.set(rcKey(p.row, p.col), p.player);

  let submitFn = null;

  for (let gr = 0; gr < 17; gr++) {
    for (let gc = 0; gc < 17; gc++) {
      if (gr % 2 === 0 && gc % 2 === 0) {
        const row = gr / 2, col = gc / 2;
        const key = rcKey(row, col);
        const cell = document.createElement('button');
        cell.type = 'button';
        cell.className = 'board-cell';
        cell.setAttribute('data-cell', row + '-' + col);
        const actionId = legal.moves.get(key);
        if (canPlay && actionId != null) {
          cell.classList.add('legal');
          cell.dataset.action = actionId;
        }
        const pawn = pawnByCell.get(key);
        if (pawn != null) {
          const piece = document.createElement('div');
          piece.className = 'pawn ' + (pawn === 0 ? 'p0' : 'p1');
          piece.setAttribute('data-pawn', String(pawn));
          piece.textContent = pawn === 0 ? 'A' : 'B';
          cell.appendChild(piece);
        }
        boardEl.appendChild(cell);
      } else if (gr % 2 === 1 && gc % 2 === 0) {
        const hr = (gr - 1) / 2, hc = gc / 2;
        const key = rcKey(hr, hc), keyLeft = rcKey(hr, hc - 1);
        const el = document.createElement('button');
        el.type = 'button';
        el.className = 'edge-slot edge-h';
        el.setAttribute('data-edge', 'h-' + hr + '-' + hc);
        if (hAnchors.has(key)) {
          el.classList.add('wall-anchor', 'wall-on');
          const owner = wallOwners && wallOwners.h.get(key);
          if (owner === 0) el.classList.add('wall-p0');
          else if (owner === 1) el.classList.add('wall-p1');
        } else if (hAnchors.has(keyLeft)) {
          el.classList.add('wall-tail');
        } else if (hc <= BOARD_SIZE - 1) {
          let actionId = legal.hwalls.get(key);
          if (actionId == null) actionId = legal.hwalls.get(keyLeft);
          if (canPlay && actionId != null) {
            el.classList.add('legal');
            el.dataset.action = actionId;
          } else {
            el.classList.add('edge-disabled');
          }
        } else {
          el.classList.add('edge-disabled');
        }
        boardEl.appendChild(el);
      } else if (gr % 2 === 0 && gc % 2 === 1) {
        const vr = gr / 2, vc = (gc - 1) / 2;
        const key = rcKey(vr, vc), keyUp = rcKey(vr - 1, vc);
        const el = document.createElement('button');
        el.type = 'button';
        el.className = 'edge-slot edge-v';
        el.setAttribute('data-edge', 'v-' + vr + '-' + vc);
        if (vAnchors.has(key)) {
          el.classList.add('wall-anchor', 'wall-on');
          const owner = wallOwners && wallOwners.v.get(key);
          if (owner === 0) el.classList.add('wall-p0');
          else if (owner === 1) el.classList.add('wall-p1');
        } else if (vAnchors.has(keyUp)) {
          el.classList.add('wall-tail');
        } else if (vr <= BOARD_SIZE - 1) {
          let actionId = legal.vwalls.get(key);
          if (actionId == null) actionId = legal.vwalls.get(keyUp);
          if (canPlay && actionId != null) {
            el.classList.add('legal');
            el.dataset.action = actionId;
          } else {
            el.classList.add('edge-disabled');
          }
        } else {
          el.classList.add('edge-disabled');
        }
        boardEl.appendChild(el);
      } else {
        const cross = document.createElement('div');
        cross.className = 'board-cross';
        boardEl.appendChild(cross);
      }
    }
  }
}

let wallOwners = { h: new Map(), v: new Map() };

function buildWallOwnersFromFrames(frames, upToIndex) {
  const hOwners = new Map(), vOwners = new Map();
  const limit = upToIndex != null ? upToIndex + 1 : frames.length;
  for (let i = 0; i < limit; i++) {
    const f = frames[i];
    const info = f.action_info;
    if (!info || (info.type !== 'hwall' && info.type !== 'vwall')) continue;
    let actor = -1;
    if (f.actor === 'player_0') actor = 0;
    else if (f.actor === 'player_1') actor = 1;
    if (actor < 0) continue;
    const key = rcKey(info.row, info.col);
    if (info.type === 'hwall') hOwners.set(key, actor);
    else vOwners.set(key, actor);
  }
  return { h: hOwners, v: vOwners };
}

let replayFramesRef = null;

async function refreshWallOwners(sessionId, aiPlayer) {
  try {
    const data = await apiGet(API_BASE + '/' + sessionId + '/replay');
    wallOwners = buildWallOwnersFromFrames(data.frames, null);
  } catch (e) { /* keep previous */ }
}

function renderEmptyGrid(boardEl) {
  boardEl.innerHTML = '';
  for (let gr = 0; gr < 17; gr++) {
    for (let gc = 0; gc < 17; gc++) {
      if (gr % 2 === 0 && gc % 2 === 0) {
        const cell = document.createElement('div');
        cell.className = 'board-cell';
        boardEl.appendChild(cell);
      } else if (gr % 2 === 1 && gc % 2 === 1) {
        const cross = document.createElement('div');
        cross.className = 'board-cross';
        boardEl.appendChild(cross);
      } else {
        const el = document.createElement('div');
        el.className = 'edge-slot ' + (gr % 2 === 1 ? 'edge-h' : 'edge-v') + ' edge-disabled';
        boardEl.appendChild(el);
      }
    }
  }
}

function renderBoard(container, gameState, ctx) {
  container.innerHTML = '';

  const wrapper = document.createElement('div');
  wrapper.className = 'quoridor-board-wrap';

  const boardEl = document.createElement('div');
  boardEl.className = 'quoridor-board-grid';

  if (!gameState || !gameState.state) {
    renderEmptyGrid(boardEl);
    wrapper.appendChild(boardEl);
    container.appendChild(wrapper);
    return;
  }

  const gs = gameState;
  const canPlay = ctx.canPlay;
  const legal = buildLegalMaps(gs.legal_actions);

  let owners = wallOwners;
  if (gs.ply_index != null && replayFramesRef) {
    owners = buildWallOwnersFromFrames(replayFramesRef, gs.ply_index);
  }

  renderGridTo(boardEl, gs.state, canPlay, legal, owners);

  boardEl.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-action]');
    if (btn) ctx.submitAction(Number(btn.dataset.action));
  });

  wrapper.appendChild(boardEl);
  container.appendChild(wrapper);

  if (!gs.ply_index && ctx.state.sessionId) {
    refreshWallOwners(ctx.state.sessionId, ctx.state.aiPlayer);
  }
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const steps = [];
  const type = actionInfo.type;
  const actor = prevState.current_player;

  if (type === 'move') {
    const oldPawn = (prevState.state.pawns || []).find(p => p.player === actor);
    if (oldPawn) {
      steps.push({
        type: 'fly',
        from: '[data-pawn="' + actor + '"]',
        to: '[data-cell="' + actionInfo.row + '-' + actionInfo.col + '"]',
        createElement() {
          const el = document.createElement('div');
          el.className = 'anim-flying-token';
          el.style.background = actor === 0 ? '#1f2937' : '#b91c1c';
          el.textContent = actor === 0 ? 'A' : 'B';
          return el;
        },
        duration: 400,
        hideFrom: true,
      });
    }
  } else if (type === 'hwall' || type === 'vwall') {
    const dir = type === 'hwall' ? 'h' : 'v';
    steps.push({
      type: 'highlight',
      target: '[data-edge="' + dir + '-' + actionInfo.row + '-' + actionInfo.col + '"]',
      className: 'wall-placed-anim',
      duration: 600,
    });
  }

  return steps.length ? steps : null;
}

const wallsExtension = {
  render(el, gameState) {
    if (!gameState || !gameState.state) {
      el.textContent = '墙剩余：--';
      return;
    }
    const wr = gameState.state.walls_remaining || [0, 0];
    el.textContent = '墙剩余：A=' + wr[0] + ' / B=' + wr[1];
  }
};

createApp({
  gameId: 'quoridor',
  gameTitle: 'Quoridor',
  gameIntro: '先到达对侧底线获胜，可放墙但必须保留双方通路。',
  players: { min: 2, max: 2 },
  renderBoard,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  getPlayerSymbol: (humanPlayer) => humanPlayer === 0 ? 'A（黑）' : 'B（红）',
  onReplayFrames(frames) { replayFramesRef = frames; },
  onGameStart() { replayFramesRef = null; },
  extensions: [wallsExtension],
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
});
