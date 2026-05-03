import { createApp } from '/static/general/app.js';

const WIN_LINES = [
  [0,1,2],[3,4,5],[6,7,8],
  [0,3,6],[1,4,7],[2,5,8],
  [0,4,8],[2,4,6],
];

function getWinningLine(board, winner) {
  if (winner < 0) return null;
  for (const line of WIN_LINES) {
    if (line.every(i => board[i] === winner)) return line;
  }
  return null;
}

function renderBoard(container, gameState, ctx) {
  container.innerHTML = '';

  const boardEl = document.createElement('div');
  boardEl.className = 'ttt-board';

  if (!gameState) {
    for (let i = 0; i < 9; i++) {
      const btn = document.createElement('button');
      btn.className = 'ttt-cell';
      btn.disabled = true;
      boardEl.appendChild(btn);
    }
    container.appendChild(boardEl);
    return;
  }

  const board = gameState.state.board;
  const winLine = getWinningLine(board, gameState.winner);
  const gs = gameState;

  const canAct = ctx.canPlay;
  const legalSet = canAct ? new Set(gs.legal_actions) : null;

  for (let i = 0; i < 9; i++) {
    const btn = document.createElement('button');
    btn.className = 'ttt-cell';
    btn.setAttribute('data-cell', String(i));
    const val = board[i];
    if (val === 0) { btn.classList.add('x'); btn.textContent = 'X'; }
    else if (val === 1) { btn.classList.add('o'); btn.textContent = 'O'; }
    if (winLine && winLine.includes(i)) btn.classList.add('win-cell');
    if (canAct && legalSet.has(i)) {
      btn.addEventListener('click', () => ctx.submitAction(i));
    } else {
      btn.disabled = true;
    }
    boardEl.appendChild(btn);
  }

  container.appendChild(boardEl);
}

function formatMove(actionInfo, actionId) {
  if (actionInfo && actionInfo.row !== undefined) {
    return '第' + (actionInfo.row + 1) + '行第' + (actionInfo.col + 1) + '列';
  }
  if (actionId !== null && actionId !== undefined) return '动作 ' + actionId;
  return '开局';
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (actionId == null || !prevState) return null;
  return [{
    type: 'highlight',
    target: '[data-cell="' + actionId + '"]',
    className: 'anim-highlight',
    duration: 400,
  }];
}

createApp({
  gameId: 'tictactoe',
  gameTitle: '井字棋',
  gameIntro: '在 3x3 棋盘上先连成一条线即可获胜',
  players: { min: 2, max: 2 },
  renderBoard,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  getPlayerSymbol: (humanPlayer) => humanPlayer === 0 ? 'X' : 'O',
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
});
