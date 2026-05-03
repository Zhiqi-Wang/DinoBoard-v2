import { createApp } from '/static/general/app.js';

const TILE_COLORS = ['blue', 'yellow', 'red', 'black', 'white'];
const TILE_LABELS = ['蓝', '黄', '红', '黑', '白'];
const TILE_CLASSES = ['tile-blue', 'tile-yellow', 'tile-red', 'tile-black', 'tile-white'];
const FLOOR_PENALTIES = [-1, -1, -2, -2, -2, -3, -3];

const NUM_COLORS = 5;
const TARGETS_PER_COLOR = 6;
const FLOOR_TARGET = 5;

let selectedSource = -1;
let selectedColor = -1;
let currentCtx = null;
let currentCenterSource = 5;

function actionId(source, color, target) {
  return source * (NUM_COLORS * TARGETS_PER_COLOR) + color * TARGETS_PER_COLOR + target;
}

function wallColorForCell(row, col) {
  return (col - row + NUM_COLORS) % NUM_COLORS;
}

function hasLegalActionForSourceColor(legalSet, source, color) {
  for (let t = 0; t < TARGETS_PER_COLOR; t++) {
    if (legalSet.has(actionId(source, color, t))) return true;
  }
  return false;
}

function clearSelection() {
  selectedSource = -1;
  selectedColor = -1;
}

function selectSourceColor(source, color) {
  if (selectedSource === source && selectedColor === color) {
    clearSelection();
  } else {
    selectedSource = source;
    selectedColor = color;
  }
  currentCtx.rerender();
}

function selectTarget(target) {
  const legalSet = new Set(currentCtx.state.gameState.legal_actions || []);
  const aid = actionId(selectedSource, selectedColor, target);
  if (!legalSet.has(aid)) return;
  clearSelection();
  currentCtx.submitAction(aid);
}

function makeTile(colorIdx, opts) {
  opts = opts || {};
  let el;
  if (opts.clickable) {
    el = document.createElement('button');
    el.type = 'button';
    el.className = 'tile-btn ' + TILE_CLASSES[colorIdx];
  } else {
    el = document.createElement('div');
    el.className = 'tile ' + TILE_CLASSES[colorIdx];
  }
  if (opts.selected) el.classList.add('tile-selected');
  if (opts.onClick) el.addEventListener('click', opts.onClick);
  return el;
}

function makeEmptyTile() {
  const el = document.createElement('div');
  el.className = 'tile tile-empty';
  return el;
}

function makeFlyingTile(colorIdx) {
  const el = document.createElement('div');
  el.className = 'anim-flying-token azul-' + TILE_COLORS[colorIdx];
  return el;
}

function makeFlyingFPToken() {
  const el = document.createElement('div');
  el.className = 'anim-flying-token';
  el.style.background = '#f8fafc';
  el.style.color = '#0f172a';
  el.style.fontWeight = '800';
  el.textContent = '1';
  return el;
}

function renderEmptyBoard(container) {
  const wrap = document.createElement('div');
  wrap.className = 'azul-board-wrap';
  const tableArea = document.createElement('div');
  tableArea.className = 'azul-table-area';

  const factoriesWrap = document.createElement('div');
  factoriesWrap.className = 'factories-wrap';
  for (let fi = 0; fi < 5; fi++) {
    const factoryEl = document.createElement('div');
    factoryEl.className = 'factory';
    const title = document.createElement('div');
    title.className = 'factory-title';
    title.textContent = '工厂 ' + (fi + 1);
    factoryEl.appendChild(title);
    const tilesGrid = document.createElement('div');
    tilesGrid.className = 'factory-tiles';
    for (let i = 0; i < 4; i++) tilesGrid.appendChild(makeEmptyTile());
    factoryEl.appendChild(tilesGrid);
    factoriesWrap.appendChild(factoryEl);
  }
  tableArea.appendChild(factoriesWrap);

  const pool = document.createElement('div');
  pool.className = 'center-pool';
  const centerTitle = document.createElement('div');
  centerTitle.className = 'center-title';
  centerTitle.textContent = '中心池';
  pool.appendChild(centerTitle);
  const tilesWrap = document.createElement('div');
  tilesWrap.className = 'center-tiles';
  const fpSlot = document.createElement('div');
  fpSlot.className = 'center-slot empty';
  const fpGhost = document.createElement('div');
  fpGhost.className = 'first-player-token';
  fpGhost.textContent = '1';
  fpSlot.appendChild(fpGhost);
  tilesWrap.appendChild(fpSlot);
  for (let c = 0; c < NUM_COLORS; c++) {
    const slot = document.createElement('div');
    slot.className = 'center-slot empty';
    slot.appendChild(makeTile(c, {}));
    tilesWrap.appendChild(slot);
  }
  pool.appendChild(tilesWrap);
  tableArea.appendChild(pool);

  wrap.appendChild(tableArea);
  container.appendChild(wrap);
}

function renderBoard(container, gameState, ctx) {
  currentCtx = ctx;
  container.innerHTML = '';

  if (!gameState) {
    renderEmptyBoard(container);
    return;
  }

  const game = gameState.state;
  const legalSet = new Set(gameState.legal_actions || []);
  const canPlay = ctx.canPlay;
  const factories = game.factories || [];
  const centerSource = factories.length;
  currentCenterSource = centerSource;

  const wrap = document.createElement('div');
  wrap.className = 'azul-board-wrap';

  if (selectedSource >= 0 && selectedColor >= 0) {
    const msg = document.createElement('div');
    msg.className = 'selection-msg';
    const srcName = selectedSource === centerSource ? '中心池' : '工厂' + (selectedSource + 1);
    msg.textContent = '已选择：' + srcName + ' 的 ' + TILE_LABELS[selectedColor] + ' 砖块。请点击目标行或地板放置。';
    wrap.appendChild(msg);
  }

  const tableArea = document.createElement('div');
  tableArea.className = 'azul-table-area';

  // Factories
  const factoriesWrap = document.createElement('div');
  factoriesWrap.className = 'factories-wrap';

  for (let fi = 0; fi < factories.length; fi++) {
    const factory = factories[fi];
    const factoryEl = document.createElement('div');
    factoryEl.className = 'factory';
    factoryEl.setAttribute('data-factory', String(fi));

    const title = document.createElement('div');
    title.className = 'factory-title';
    title.textContent = '工厂 ' + (fi + 1);
    factoryEl.appendChild(title);

    const tilesGrid = document.createElement('div');
    tilesGrid.className = 'factory-tiles';

    let totalTiles = 0;
    for (let c = 0; c < NUM_COLORS; c++) totalTiles += (factory[c] || 0);

    if (totalTiles === 0) {
      for (let i = 0; i < 4; i++) tilesGrid.appendChild(makeEmptyTile());
    } else {
      for (let c = 0; c < NUM_COLORS; c++) {
        const count = factory[c] || 0;
        for (let ti = 0; ti < count; ti++) {
          const selectable = canPlay && hasLegalActionForSourceColor(legalSet, fi, c);
          const sel = selectedSource === fi && selectedColor === c;
          const tile = makeTile(c, {
            clickable: selectable,
            selected: sel,
            onClick: selectable ? ((s, cl) => () => selectSourceColor(s, cl))(fi, c) : null,
          });
          tilesGrid.appendChild(tile);
        }
      }
    }
    factoryEl.appendChild(tilesGrid);
    factoriesWrap.appendChild(factoryEl);
  }
  tableArea.appendChild(factoriesWrap);

  // Center pool: 6 fixed slots (FP token + 5 colors)
  const pool = document.createElement('div');
  pool.className = 'center-pool';

  const centerTitle = document.createElement('div');
  centerTitle.className = 'center-title';
  centerTitle.textContent = '中心池';
  pool.appendChild(centerTitle);

  const center = game.center || [];
  const tilesWrap = document.createElement('div');
  tilesWrap.className = 'center-tiles';

  // First-player token slot (always rendered)
  const fpSlot = document.createElement('div');
  fpSlot.className = 'center-slot';
  fpSlot.setAttribute('data-center-slot', 'fp');
  if (game.first_player_token_in_center) {
    const token = document.createElement('div');
    token.className = 'first-player-token';
    token.textContent = '1';
    fpSlot.appendChild(token);
  } else {
    fpSlot.classList.add('empty');
    const ghost = document.createElement('div');
    ghost.className = 'first-player-token';
    ghost.textContent = '1';
    fpSlot.appendChild(ghost);
  }
  tilesWrap.appendChild(fpSlot);

  // 5 color slots (always rendered)
  for (let c = 0; c < NUM_COLORS; c++) {
    const count = center[c] || 0;
    const slot = document.createElement('div');
    slot.className = 'center-slot';
    slot.setAttribute('data-center-slot', String(c));

    if (count > 0) {
      const selectable = canPlay && hasLegalActionForSourceColor(legalSet, centerSource, c);
      const sel = selectedSource === centerSource && selectedColor === c;
      slot.appendChild(makeTile(c, {
        clickable: selectable,
        selected: sel,
        onClick: selectable ? ((cl) => () => selectSourceColor(centerSource, cl))(c) : null,
      }));
      if (count > 1) {
        const badge = document.createElement('div');
        badge.className = 'center-count';
        badge.textContent = String(count);
        slot.appendChild(badge);
      }
    } else {
      slot.classList.add('empty');
      slot.appendChild(makeTile(c, {}));
    }
    tilesWrap.appendChild(slot);
  }
  pool.appendChild(tilesWrap);
  tableArea.appendChild(pool);
  wrap.appendChild(tableArea);
  container.appendChild(wrap);
}

function renderEmptyPlayerArea(container, numPlayers) {
  const wrap = document.createElement('div');
  wrap.className = 'azul-players-area';
  if (numPlayers > 2) wrap.classList.add('players-multi');
  for (let pi = 0; pi < numPlayers; pi++) {
    const boardEl = document.createElement('div');
    boardEl.className = 'player-board';
    const titleEl = document.createElement('h3');
    titleEl.textContent = '玩家' + pi;
    boardEl.appendChild(titleEl);

    const gridEl = document.createElement('div');
    gridEl.className = 'board-grid';
    const patternEl = document.createElement('div');
    patternEl.className = 'pattern-panel';
    for (let row = 0; row < 5; row++) {
      const rowEl = document.createElement('div');
      rowEl.className = 'pattern-row target-slot slot-disabled';
      for (let ci = 0; ci <= row; ci++) rowEl.appendChild(makeEmptyTile());
      patternEl.appendChild(rowEl);
    }
    gridEl.appendChild(patternEl);

    const wallEl = document.createElement('div');
    wallEl.className = 'wall-panel';
    for (let row = 0; row < 5; row++) {
      const wallRowEl = document.createElement('div');
      wallRowEl.className = 'wall-row';
      for (let col = 0; col < 5; col++) {
        const colorIdx = wallColorForCell(row, col);
        const cell = document.createElement('div');
        cell.className = 'wall-cell ghost ' + TILE_CLASSES[colorIdx];
        cell.style.opacity = '0.2';
        wallRowEl.appendChild(cell);
      }
      wallEl.appendChild(wallRowEl);
    }
    gridEl.appendChild(wallEl);
    boardEl.appendChild(gridEl);

    const floorArea = document.createElement('div');
    floorArea.className = 'floor-area';
    const floorLabel = document.createElement('span');
    floorLabel.className = 'floor-label';
    floorLabel.textContent = '地板';
    floorArea.appendChild(floorLabel);
    const floorRow = document.createElement('div');
    floorRow.className = 'floor-row';
    for (let fi = 0; fi < 7; fi++) {
      const slotEl = document.createElement('div');
      slotEl.className = 'floor-slot';
      const base = document.createElement('div');
      base.className = 'floor-base';
      base.textContent = String(FLOOR_PENALTIES[fi]);
      slotEl.appendChild(base);
      floorRow.appendChild(slotEl);
    }
    floorArea.appendChild(floorRow);
    boardEl.appendChild(floorArea);
    wrap.appendChild(boardEl);
  }
  container.appendChild(wrap);
}

function renderPlayerArea(container, gameState, ctx) {
  currentCtx = ctx;
  container.innerHTML = '';

  const numPlayers = gameState ? (gameState.num_players || 2) : 2;

  if (!gameState) {
    renderEmptyPlayerArea(container, numPlayers);
    return;
  }

  const game = gameState.state;
  const appState = ctx.state;
  const legalSet = new Set(gameState.legal_actions || []);
  const canPlay = ctx.canPlay;
  const hasSelection = selectedSource >= 0 && selectedColor >= 0;
  const players = game.players || [];

  const wrap = document.createElement('div');
  wrap.className = 'azul-players-area';
  if (numPlayers > 2) wrap.classList.add('players-multi');

  for (let pi = 0; pi < numPlayers; pi++) {
    const pd = players[pi] || {};
    const isHuman = pi === appState.humanPlayer;
    const isCurrent = gameState.current_player === pi;

    const boardEl = document.createElement('div');
    boardEl.className = 'player-board' + (isCurrent ? ' active-turn' : '');
    boardEl.setAttribute('data-player', String(pi));

    const titleEl = document.createElement('h3');
    titleEl.textContent = '玩家' + pi + (isHuman ? '（你）' : '') +
      (isCurrent ? ' · 当前' : '') + ' · 分数: ' + (pd.score || 0);
    boardEl.appendChild(titleEl);

    const shouldHighlight = hasSelection && canPlay && ((isHuman && !appState.forceMode) || (appState.forceMode && isCurrent));

    const gridEl = document.createElement('div');
    gridEl.className = 'board-grid';

    // Pattern lines
    const patternEl = document.createElement('div');
    patternEl.className = 'pattern-panel';
    const lines = pd.pattern_lines || [];

    for (let row = 0; row < 5; row++) {
      const line = lines[row] || { capacity: row + 1, color: -1, length: 0 };
      const capacity = line.capacity || (row + 1);
      const lineColor = line.color;
      const lineLen = line.length || 0;

      const rowEl = document.createElement('div');
      rowEl.className = 'pattern-row';
      rowEl.setAttribute('data-pattern-row', pi + '-' + row);

      const targetAid = hasSelection ? actionId(selectedSource, selectedColor, row) : -1;
      const isLegalTarget = shouldHighlight && legalSet.has(targetAid);

      if (isLegalTarget) {
        rowEl.classList.add('target-slot', 'slot-highlight');
        rowEl.addEventListener('click', ((t) => () => selectTarget(t))(row));
      } else {
        rowEl.classList.add('target-slot', 'slot-disabled');
      }

      const emptyCount = capacity - lineLen;
      for (let ci = 0; ci < emptyCount; ci++) {
        rowEl.appendChild(makeEmptyTile());
      }
      for (let ci = 0; ci < lineLen; ci++) {
        if (lineColor >= 0) {
          rowEl.appendChild(makeTile(lineColor, {}));
        } else {
          rowEl.appendChild(makeEmptyTile());
        }
      }
      patternEl.appendChild(rowEl);
    }
    gridEl.appendChild(patternEl);

    // Wall
    const wallEl = document.createElement('div');
    wallEl.className = 'wall-panel';
    const wall = pd.wall || [];

    for (let row = 0; row < 5; row++) {
      const wallRowEl = document.createElement('div');
      wallRowEl.className = 'wall-row';
      const wallRow = wall[row] || [0, 0, 0, 0, 0];

      for (let col = 0; col < 5; col++) {
        const colorIdx = wallColorForCell(row, col);
        const cell = document.createElement('div');
        cell.className = 'wall-cell';
        cell.setAttribute('data-wall-cell', pi + '-' + row + '-' + col);

        if (wallRow[col]) {
          cell.classList.add('filled', TILE_CLASSES[colorIdx]);
        } else {
          cell.classList.add('ghost', TILE_CLASSES[colorIdx]);
        }
        wallRowEl.appendChild(cell);
      }
      wallEl.appendChild(wallRowEl);
    }
    gridEl.appendChild(wallEl);
    boardEl.appendChild(gridEl);

    // Floor
    const floorArea = document.createElement('div');
    floorArea.className = 'floor-area';

    const floorLabel = document.createElement('span');
    floorLabel.className = 'floor-label';
    floorLabel.textContent = '地板';
    floorArea.appendChild(floorLabel);

    const floorRow = document.createElement('div');
    floorRow.className = 'floor-row';
    const floor = pd.floor || [];
    const floorCount = pd.floor_count || 0;

    const floorTargetAid = hasSelection ? actionId(selectedSource, selectedColor, FLOOR_TARGET) : -1;
    const floorIsLegal = shouldHighlight && legalSet.has(floorTargetAid);

    for (let fi = 0; fi < 7; fi++) {
      const slotEl = document.createElement('div');
      slotEl.className = 'floor-slot';
      slotEl.setAttribute('data-floor-slot', pi + '-' + fi);

      if (floorIsLegal && fi === floorCount) {
        slotEl.classList.add('floor-target', 'slot-highlight');
        slotEl.addEventListener('click', () => selectTarget(FLOOR_TARGET));
      }

      const base = document.createElement('div');
      base.className = 'floor-base';
      base.textContent = String(FLOOR_PENALTIES[fi]);
      slotEl.appendChild(base);

      if (fi < floorCount && fi < floor.length) {
        const tileColor = floor[fi];
        if (tileColor >= 0 && tileColor < NUM_COLORS) {
          const tileEl = document.createElement('div');
          tileEl.className = 'floor-tile ' + TILE_CLASSES[tileColor];
          slotEl.appendChild(tileEl);
        } else if (tileColor === -2) {
          const tokenEl = document.createElement('div');
          tokenEl.className = 'floor-tile first-player-floor-token';
          tokenEl.textContent = '1';
          slotEl.appendChild(tokenEl);
        }
      }
      floorRow.appendChild(slotEl);
    }
    floorArea.appendChild(floorRow);
    boardEl.appendChild(floorArea);

    wrap.appendChild(boardEl);
  }

  container.appendChild(wrap);
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const steps = [];
  const prev = prevState.state;
  const actor = prevState.current_player;
  const source = actionInfo.source;
  const color = actionInfo.color;
  const targetLine = actionInfo.target_line;
  const isCenter = actionInfo.is_center;
  const centerSource = (prev.factories || []).length;

  const fromSelector = isCenter
    ? '[data-center-slot="' + color + '"]'
    : '[data-factory="' + source + '"]';

  const toSelector = targetLine < 5
    ? '[data-pattern-row="' + actor + '-' + targetLine + '"]'
    : '[data-floor-slot="' + actor + '-' + (prev.players[actor].floor_count || 0) + '"]';

  // Tiles fly from source to target
  steps.push({
    type: 'fly',
    from: fromSelector,
    to: toSelector,
    createElement: () => makeFlyingTile(color),
    duration: 350,
    hideFrom: true,
  });

  // Factory remnants fly to center (only when taking from factory)
  if (!isCenter) {
    const factory = prev.factories[source];
    for (let c = 0; c < NUM_COLORS; c++) {
      if (c === color) continue;
      const cnt = factory[c] || 0;
      if (cnt > 0) {
        steps.push({
          type: 'fly',
          from: fromSelector,
          to: '[data-center-slot="' + c + '"]',
          createElement: () => makeFlyingTile(c),
          duration: 350,
        });
      }
    }
  }

  // First-player token flies to floor (first take from center in a round)
  if (isCenter && prev.first_player_token_in_center) {
    const floorIdx = prev.players[actor].floor_count || 0;
    steps.push({
      type: 'fly',
      from: '[data-center-slot="fp"]',
      to: '[data-floor-slot="' + actor + '-' + floorIdx + '"]',
      createElement: makeFlyingFPToken,
      duration: 350,
      hideFrom: true,
    });
  }

  return steps.length ? steps : null;
}

function formatMove(actionInfo, aid) {
  if (!actionInfo && (aid === null || aid === undefined)) return '开局';
  if (!actionInfo) return '动作 #' + aid;
  const srcName = actionInfo.is_center ? '中心池' : '工厂' + (actionInfo.source + 1);
  const colorName = TILE_LABELS[actionInfo.color] || '?';
  const targetName = actionInfo.target_line < NUM_COLORS ? '第' + (actionInfo.target_line + 1) + '行' : '地板';
  return srcName + ' ' + colorName + ' → ' + targetName;
}

const bagExtension = {
  render(el, gameState) {
    if (!gameState || !gameState.state) {
      el.textContent = '袋中剩余：--';
      return;
    }
    const game = gameState.state;
    const bagCounts = game.bag_counts || [];
    const bagTotal = game.bag_total || 0;
    const parts = [];
    for (let c = 0; c < NUM_COLORS; c++) {
      parts.push(TILE_LABELS[c] + ':' + (bagCounts[c] || 0));
    }
    el.textContent = '袋中剩余：' + bagTotal + '（' + parts.join(' ') + '）· 第' + ((game.round_index || 0) + 1) + '轮';
  }
};

const scoreExtension = {
  render(el, gameState) {
    if (!gameState || !gameState.state) {
      el.textContent = '当前分数：--';
      return;
    }
    const scores = gameState.state.scores || [];
    const parts = scores.map((s, i) => 'P' + i + ' ' + s);
    el.textContent = '当前分数：' + parts.join(' / ');
  }
};

createApp({
  gameId: 'azul',
  gameTitle: 'Azul 花砖物语',
  gameIntro: '从工厂或中心拿同色砖块，放入花纹线或地板。',
  players: { min: 2, max: 4 },
  renderBoard,
  renderPlayerArea,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  extensions: [scoreExtension, bagExtension],
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
  onActionSubmitted() { clearSelection(); },
  onGameStart() { clearSelection(); },
  onUndo() { clearSelection(); },
});
