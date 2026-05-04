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
  tableArea.appendChild(factoriesWrap);
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

  // Selection prompt lives in the info panel's reserved status slot
  // (position 2) instead of being prepended to the board, so triggering
  // a selection doesn't push the rest of the board down by one row.
  if (ctx && ctx.setInfoStatus) {
    if (selectedSource >= 0 && selectedColor >= 0) {
      const srcName = selectedSource === centerSource ? '中心池' : '工厂' + (selectedSource + 1);
      ctx.setInfoStatus('已选 ' + srcName + ' ' + TILE_LABELS[selectedColor] + ' — 点击目标行/地板');
    } else {
      ctx.setInfoStatus(null);
    }
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

  // Center pool: 6 fixed slots (FP token + 5 colors). Rendered as a
  // grid-sibling of the factories so the public area reads as one
  // spatial block rather than a side-docked panel.
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
  tableArea.appendChild(factoriesWrap);
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
    // "当前" dropped — active-turn is already signaled by the .active-turn
    // class on .player-board (visual highlight). Duplicating it in the
    // title just adds noise.
    titleEl.textContent = '玩家' + pi + (isHuman ? '（你）' : '') +
      ' · 分数: ' + (pd.score || 0);
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

    // Whole-row click: mirror the pattern-row behavior so the player can
    // drop tiles on the floor by clicking anywhere on the row, not just
    // on the next-empty slot.
    if (floorIsLegal) {
      floorRow.classList.add('floor-target', 'slot-highlight');
      floorRow.addEventListener('click', () => selectTarget(FLOOR_TARGET));
    }

    for (let fi = 0; fi < 7; fi++) {
      const slotEl = document.createElement('div');
      slotEl.className = 'floor-slot';
      slotEl.setAttribute('data-floor-slot', pi + '-' + fi);

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

// Tile size matches the board tile CSS (.tile is 40x40). Explicit here so
// the flying sprite doesn't inherit the factory container's size (~140px)
// when flying from a factory. Must equal .tile width so the sprite's
// apparent size matches rendered tiles at both ends of the flight.
const FLY_TILE_SIZE = 40;
const FLY_DURATION = 350;
const SETTLE_PAUSE = 200;  // between action animation and round-end settlement

// Wall column layout: in Azul, the wall cell for color c in row r sits
// at column (c + r) mod 5. Matches the C++ rule in AzulRules.
function wallColForColor(row, color) {
  return (color + row) % NUM_COLORS;
}

// Approximate placement score used by the animation popup — matches the
// C++ apply_round_settlement logic: count contiguous filled cells in the
// row direction and column direction that include the just-placed cell.
// If both > 1, the score is the sum; otherwise it's max(row-run, col-run, 1).
function computePlacementScore(wall, row, col) {
  let rowRun = 1;
  for (let c = col - 1; c >= 0 && wall[row][c]; c--) rowRun++;
  for (let c = col + 1; c < 5 && wall[row][c]; c++) rowRun++;
  let colRun = 1;
  for (let r = row - 1; r >= 0 && wall[r][col]; r--) colRun++;
  for (let r = row + 1; r < 5 && wall[r][col]; r++) colRun++;
  if (rowRun > 1 && colRun > 1) return rowRun + colRun;
  return Math.max(rowRun, colRun, 1);
}

// Aggregate floor penalty for N tiles on the floor (FLOOR_PENALTIES[0..6]).
function floorPenaltyTotal(count) {
  let p = 0;
  for (let i = 0; i < count && i < FLOOR_PENALTIES.length; i++) p += FLOOR_PENALTIES[i];
  return p;
}

// Transform a center-pool slot to look like its "empty ghost" state.
// Used as an onStart hook on the fly that takes tiles out of the center
// — hideFrom would hide the whole slot (including the ghost background),
// leaving a hole in the grid. This keeps the slot visually in place but
// in its post-take appearance.
function transformCenterSlotToEmpty(slotEl, color) {
  slotEl.innerHTML = '';
  slotEl.classList.add('empty');
  const ghost = document.createElement('div');
  ghost.className = 'tile ' + TILE_CLASSES[color];
  slotEl.appendChild(ghost);
}

function transformFpSlotToEmpty(slotEl) {
  slotEl.innerHTML = '';
  slotEl.classList.add('empty');
  const ghost = document.createElement('div');
  ghost.className = 'first-player-token';
  ghost.textContent = '1';
  slotEl.appendChild(ghost);
}

// Transform a pattern row to look empty (capacity × empty tile placeholders).
// Used as onStart on the settle fly (pattern → wall): without this, the
// whole row including its slot-boxes vanishes via hideFrom and users see
// a hole in the player's board. With this, the row slots stay visible
// empty, matching state_after.
function transformPatternRowToEmpty(rowEl, capacity) {
  rowEl.innerHTML = '';
  for (let i = 0; i < capacity; i++) {
    const t = document.createElement('div');
    t.className = 'tile tile-empty';
    rowEl.appendChild(t);
  }
}

function mainActionFlights(prev, actor, source, color, targetLine, isCenter,
                           roundEndingOrGameEnd) {
  // For factory source, fly from the inner .factory-tiles grid — hideFrom
  // then hides only the tile sprites, leaving the factory disc + title
  // visible. Previously we flew from the whole factory element, which
  // hid the entire factory (including title) and looked broken.
  const fromSelector = isCenter
    ? '[data-center-slot="' + color + '"]'
    : '[data-factory="' + source + '"] .factory-tiles';

  // For floor placement from center with FP token present, FP lands at
  // slot `floor_count` (first available); shift the main tile one slot
  // further so the two sprites don't overlap at the same destination.
  const fpAlsoToFloor = isCenter && !!prev.first_player_token_in_center;
  const floorLandIdx = (prev.players[actor].floor_count || 0)
    + (targetLine === 5 && fpAlsoToFloor ? 1 : 0);
  const toSelector = targetLine < 5
    ? '[data-pattern-row="' + actor + '-' + targetLine + '"]'
    : '[data-floor-slot="' + actor + '-' + floorLandIdx + '"]';

  const flights = [];

  // When this action triggers round-end settlement, patch the DOM at
  // main-sprite landing time to show the "post-action, pre-settlement"
  // intermediate state. Without this, re-render after animation shows
  // the post-settlement state (row cleared to wall, floor cleared to
  // discard) and users read it as "the tile I just placed disappeared".
  //
  // Two scenarios handled:
  //  - Pattern row placement (targetLine < 5): show actual post-action
  //    tile count (prev length + tiles placed, capped at capacity), not
  //    unconditionally `capacity`. Overflow tiles (when the row fills
  //    and excess tiles drop) are painted onto the floor slots.
  //  - Floor placement (targetLine === 5): paint the placed tiles onto
  //    the floor starting at prev floor count. Otherwise the sprite
  //    lands on the floor slot but nothing persists — re-render then
  //    clears the floor (as round-end does), so the tile visibly
  //    vanishes without ever appearing to rest on the floor.
  let onMainLand = null;
  if (roundEndingOrGameEnd) {
    onMainLand = () => {
      const prevP = prev.players[actor] || {};
      const prevFloorCount = prevP.floor_count || 0;
      const tilesTaken = isCenter
          ? ((prev.center || [])[color] || 0)
          : ((prev.factories || [])[source] || [])[color] || 0;
      // FP token (when present in center) also lands on floor, at the
      // first-available floor slot. A separate FP flight (added below)
      // flies to slot `prevFloorCount`; so we offset our floor paint by
      // one to avoid overwriting the FP slot content.
      const fpArriving = isCenter && !!prev.first_player_token_in_center;
      const floorStart = prevFloorCount + (fpArriving ? 1 : 0);

      let overflow = 0;
      if (targetLine < 5) {
        const capacity = targetLine + 1;
        const line = (prevP.pattern_lines || [])[targetLine] || { length: 0 };
        const prevLen = line.length || 0;
        const placed = Math.min(tilesTaken, Math.max(0, capacity - prevLen));
        overflow = tilesTaken - placed;
        const postLen = prevLen + placed;

        const rowEl = document.querySelector(
          '[data-pattern-row="' + actor + '-' + targetLine + '"]');
        if (rowEl) {
          rowEl.innerHTML = '';
          for (let i = 0; i < capacity - postLen; i++) {
            const e = document.createElement('div');
            e.className = 'tile tile-empty';
            rowEl.appendChild(e);
          }
          for (let i = 0; i < postLen; i++) {
            const t = document.createElement('div');
            t.className = 'tile ' + TILE_CLASSES[color];
            rowEl.appendChild(t);
          }
        }
      } else {
        overflow = tilesTaken;
      }

      for (let i = 0; i < overflow; i++) {
        const slotIdx = floorStart + i;
        if (slotIdx >= 7) break;
        const slotEl = document.querySelector(
          '[data-floor-slot="' + actor + '-' + slotIdx + '"]');
        if (!slotEl) continue;
        const existing = slotEl.querySelector('.floor-tile');
        if (existing) existing.remove();
        const t = document.createElement('div');
        t.className = 'floor-tile ' + TILE_CLASSES[color];
        slotEl.appendChild(t);
      }
    };
  }

  // Main flight. When source is a center slot, transform it to its empty
  // ghost state on start (not hideFrom) so the slot grid stays visible.
  // When source is a factory's tile grid, hideFrom is fine — the factory
  // disc + title wrap stays visible, and the tile grid empty state naturally
  // appears on re-render.
  const mainFlight = {
    from: fromSelector, to: toSelector,
    createElement: () => makeFlyingTile(color),
    duration: FLY_DURATION, width: FLY_TILE_SIZE, height: FLY_TILE_SIZE,
    onComplete: onMainLand,
  };
  if (isCenter) {
    mainFlight.onStart = (el) => transformCenterSlotToEmpty(el, color);
  } else {
    mainFlight.hideFrom = true;
  }
  flights.push(mainFlight);

  if (!isCenter) {
    const factory = prev.factories[source];
    for (let c = 0; c < NUM_COLORS; c++) {
      if (c === color) continue;
      const cnt = factory[c] || 0;
      if (cnt > 0) {
        flights.push({
          from: fromSelector, to: '[data-center-slot="' + c + '"]',
          createElement: () => makeFlyingTile(c),
          duration: FLY_DURATION, width: FLY_TILE_SIZE, height: FLY_TILE_SIZE,
        });
      }
    }
  }

  if (isCenter && prev.first_player_token_in_center) {
    const floorIdx = prev.players[actor].floor_count || 0;
    flights.push({
      from: '[data-center-slot="fp"]',
      to: '[data-floor-slot="' + actor + '-' + floorIdx + '"]',
      createElement: makeFlyingFPToken,
      duration: FLY_DURATION, width: FLY_TILE_SIZE, height: FLY_TILE_SIZE,
      onStart: (el) => transformFpSlotToEmpty(el),
    });
  }
  return flights;
}

// Identify every (player, row, col, color) tile that was placed onto a
// wall during settlement this transition. Walk the wall diff rather than
// the pattern_lines diff: a pattern line filled BY this action and then
// settled shows up as "was empty, is empty" in pattern_lines (pre-action
// snapshot vs post-settlement snapshot) and would be missed by the
// naive "was full, now empty" check — but the wall diff catches it
// cleanly.
function settledPlacements(prev, next, numPlayers) {
  const placements = [];
  for (let pi = 0; pi < numPlayers; pi++) {
    const prevWall = (prev.players[pi] && prev.players[pi].wall) || [];
    const newWall = (next.players[pi] && next.players[pi].wall) || [];
    for (let row = 0; row < 5; row++) {
      const pr = prevWall[row] || [];
      const nr = newWall[row] || [];
      for (let col = 0; col < 5; col++) {
        if (!pr[col] && nr[col]) {
          // Color for this (row, col): inverse of the wall layout rule.
          const color = (col - row + NUM_COLORS) % NUM_COLORS;
          placements.push({ pi, row, col, color });
        }
      }
    }
  }
  return placements;
}

function roundEndSteps(prev, next, numPlayers) {
  const placements = settledPlacements(prev, next, numPlayers);

  const flights = [];
  const popups = [];

  for (const { pi, row, col, color } of placements) {
    const capacity = row + 1;
    flights.push({
      from: '[data-pattern-row="' + pi + '-' + row + '"]',
      to: '[data-wall-cell="' + pi + '-' + row + '-' + col + '"]',
      createElement: () => makeFlyingTile(color),
      duration: FLY_DURATION, width: FLY_TILE_SIZE, height: FLY_TILE_SIZE,
      // Instead of hiding the row (which erases the slot grid visually),
      // swap its content to `capacity` empty tile placeholders so the
      // slots stay in place while the colored sprite flies to the wall.
      onStart: (el) => transformPatternRowToEmpty(el, capacity),
    });
    const nextWall = next.players[pi].wall;
    const score = computePlacementScore(nextWall, row, col);
    popups.push({
      type: 'popup',
      target: '[data-wall-cell="' + pi + '-' + row + '-' + col + '"]',
      content: '+' + score,
      duration: 1800,
    });
  }

  // Floor penalty popup — one per player that had anything on the floor.
  for (let pi = 0; pi < numPlayers; pi++) {
    const prevP = prev.players[pi];
    if (!prevP) continue;
    const floorCount = prevP.floor_count || 0;
    if (floorCount <= 0) continue;
    const penalty = floorPenaltyTotal(floorCount);
    if (penalty === 0) continue;
    popups.push({
      type: 'popup',
      target: '[data-player="' + pi + '"] .floor-row',
      content: String(penalty),  // already negative
      className: 'popup-penalty',
      duration: 1800,
    });
  }

  const steps = [];
  // The current actor's target pattern row is patched inside the main
  // fly's onComplete (see mainActionFlights), so the transition from
  // "sprite lands" → "row looks full" is gap-less. Other players'
  // settling rows were already rendered full in state_before.
  // We just need a short hold so the user can see the full row(s)
  // before they fly off to the wall.
  if (placements.length) {
    steps.push({ type: 'pause', duration: 300 });
    steps.push({ type: 'flyGroup', flights });
  }
  if (popups.length) {
    steps.push({ type: 'group', children: popups });
  }
  return { steps };
}

function gameEndSteps(next, numPlayers) {
  // Count row / column / color completions on each player's final wall
  // and flash-highlight them with a bonus popup.
  const highlights = [];
  const popups = [];

  for (let pi = 0; pi < numPlayers; pi++) {
    const wall = (next.players[pi] && next.players[pi].wall) || [];
    if (!wall.length) continue;

    // Rows: +2 each
    for (let row = 0; row < 5; row++) {
      const r = wall[row] || [];
      if (r.length === 5 && r.every(Boolean)) {
        for (let col = 0; col < 5; col++) {
          highlights.push({
            type: 'highlight',
            target: '[data-wall-cell="' + pi + '-' + row + '-' + col + '"]',
            className: 'anim-bonus-flash',
            duration: 900,
          });
        }
        popups.push({
          type: 'popup',
          target: '[data-wall-cell="' + pi + '-' + row + '-2"]',
          content: '+2', className: 'popup-bonus', duration: 2200,
        });
      }
    }

    // Columns: +7 each
    for (let col = 0; col < 5; col++) {
      let full = true;
      for (let row = 0; row < 5; row++) {
        if (!(wall[row] && wall[row][col])) { full = false; break; }
      }
      if (full) {
        for (let row = 0; row < 5; row++) {
          highlights.push({
            type: 'highlight',
            target: '[data-wall-cell="' + pi + '-' + row + '-' + col + '"]',
            className: 'anim-bonus-flash',
            duration: 900,
          });
        }
        popups.push({
          type: 'popup',
          target: '[data-wall-cell="' + pi + '-2-' + col + '"]',
          content: '+7', className: 'popup-bonus', duration: 2200,
        });
      }
    }

    // Colors: +10 each. Wall cell for color c in row r is col (c+r)%5.
    for (let color = 0; color < NUM_COLORS; color++) {
      let full = true;
      const cells = [];
      for (let row = 0; row < 5; row++) {
        const col = wallColForColor(row, color);
        if (!(wall[row] && wall[row][col])) { full = false; break; }
        cells.push({ row, col });
      }
      if (full) {
        for (const { row, col } of cells) {
          highlights.push({
            type: 'highlight',
            target: '[data-wall-cell="' + pi + '-' + row + '-' + col + '"]',
            className: 'anim-bonus-flash',
            duration: 900,
          });
        }
        const mid = cells[2];
        popups.push({
          type: 'popup',
          target: '[data-wall-cell="' + pi + '-' + mid.row + '-' + mid.col + '"]',
          content: '+10', className: 'popup-bonus', duration: 2200,
        });
      }
    }
  }

  const steps = [];
  if (highlights.length) steps.push({ type: 'group', children: highlights });
  if (popups.length) steps.push({ type: 'group', children: popups });
  return steps;
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state || !newState || !newState.state) return null;

  const prev = prevState.state;
  const next = newState.state;
  const actor = prevState.current_player;
  const source = actionInfo.source;
  const color = actionInfo.color;
  const targetLine = actionInfo.target_line;
  const isCenter = actionInfo.is_center;
  const numPlayers = newState.num_players || prev.players.length || 2;

  const steps = [];

  const roundEnded = (next.round_index || 0) > (prev.round_index || 0);
  const gameEnded = !!newState.is_terminal;

  // Phase 1: the action itself — all tile moves happen in parallel with
  // real tile size (not inherited from the factory container — see BUG-021).
  // The main flight's onComplete patches the pattern row to look full at
  // landing time if round-end is about to fire — eliminates the flicker
  // where the just-placed tile appears to vanish right before settlement.
  const actionFlights = mainActionFlights(
      prev, actor, source, color, targetLine, isCenter,
      roundEnded || gameEnded);
  if (actionFlights.length) {
    steps.push({ type: 'flyGroup', flights: actionFlights });
  }

  // Phase 2: round-end settlement. Pattern lines → wall + floor penalty
  // popups. Only plays when the round actually ended (the C++ settlement
  // ran inside do_action_fast).
  if (roundEnded || gameEnded) {
    steps.push({ type: 'pause', duration: SETTLE_PAUSE });
    const settle = roundEndSteps(prev, next, numPlayers);
    steps.push(...settle.steps);
  }

  // Phase 3: end-game bonuses. Highlight complete rows / cols / colors.
  if (gameEnded) {
    steps.push({ type: 'pause', duration: 300 });
    const bonus = gameEndSteps(next, numPlayers);
    steps.push(...bonus);
  }

  // Phase 4: factory refill is not animated explicitly — the new factory
  // contents just appear on re-render. Adding a refill animation would
  // make the end-of-round sequence feel too long; left out intentionally.

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
    const bagCounts = gameState.state.bag_counts || [];
    const parts = [];
    for (let c = 0; c < NUM_COLORS; c++) {
      parts.push(TILE_LABELS[c] + (bagCounts[c] || 0));
    }
    el.textContent = '袋中剩余：' + parts.join(' ');
  }
};

// scoreExtension was previously shown here but scores are already
// rendered in each player's board — duplicating them as an info-panel
// pill added noise without new information.

createApp({
  gameId: 'azul',
  gameTitle: 'Azul 花砖物语',
  gameIntro: '先选择来源再选择目标行',
  players: { min: 2, max: 4 },
  renderBoard,
  renderPlayerArea,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  extensions: [bagExtension],
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
  onActionSubmitted() { clearSelection(); },
  onGameStart() { clearSelection(); },
  onUndo() { clearSelection(); },
});
