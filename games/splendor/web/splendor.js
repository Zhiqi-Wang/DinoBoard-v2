import { createApp } from '/static/general/app.js';

const COLOR_NAMES = ['white', 'blue', 'green', 'red', 'black', 'gold'];
const COLOR_LABELS = ['白', '蓝', '绿', '红', '黑', '金'];
const TAKE_THREE_COMBOS = [
  [0,1,2],[0,1,3],[0,1,4],[0,2,3],[0,2,4],[0,3,4],
  [1,2,3],[1,2,4],[1,3,4],[2,3,4]
];
const TAKE_TWO_DIFF_COMBOS = [
  [0,1],[0,2],[0,3],[0,4],[1,2],[1,3],[1,4],[2,3],[2,4],[3,4]
];

const BUY_FACEUP = 0, RESERVE_FACEUP = 12, RESERVE_DECK = 24, BUY_RESERVED = 27;
const TAKE_THREE = 30, TAKE_TWO_DIFF = 40, TAKE_ONE = 50, TAKE_TWO_SAME = 55;
const CHOOSE_NOBLE = 60, RETURN_TOKEN = 63, PASS_ACTION = 69;

let gemPick = [];
let currentCtx = null;

function getLegalSet(gameState) {
  if (!gameState) return new Set();
  return new Set(gameState.legal_actions || []);
}


function getStage(gameState) {
  return gameState && gameState.state ? (gameState.state.stage || 0) : 0;
}

function formatActionInfo(info, actionId) {
  if (!info || !info.type) return '动作 #' + actionId;
  switch (info.type) {
    case 'buy_faceup': return '购买 T' + (info.tier + 1) + ' #' + (info.slot + 1);
    case 'reserve_faceup': return '保留明牌 T' + (info.tier + 1) + ' #' + (info.slot + 1);
    case 'reserve_deck': return '保留牌堆 T' + (info.tier + 1);
    case 'buy_reserved': return '购买保留牌 #' + (info.slot + 1);
    case 'take_three': return '拿三色：' + (info.colors || []).map(c => COLOR_LABELS[c]).join('+');
    case 'take_two_different': return '拿两色：' + (info.colors || []).map(c => COLOR_LABELS[c]).join('+');
    case 'take_one': return '拿一枚：' + COLOR_LABELS[info.color];
    case 'take_two_same': return '拿两枚同色：' + COLOR_LABELS[info.color];
    case 'choose_noble': return '选择贵族 #' + (info.noble_slot + 1);
    case 'return_token': return '返还：' + COLOR_LABELS[info.token];
    case 'pass': return '跳过回合';
    default: return '动作 #' + actionId;
  }
}

// --- Gem picking logic ---

function resolveTakeActionId(colors, legalSet) {
  if (!colors || colors.length === 0) return null;
  if (colors.length === 2 && colors[0] === colors[1]) {
    const aid = TAKE_TWO_SAME + colors[0];
    return legalSet.has(aid) ? aid : null;
  }
  const uniq = Array.from(new Set(colors));
  if (uniq.length !== colors.length) return null;
  if (uniq.length === 1) { const aid = TAKE_ONE + uniq[0]; return legalSet.has(aid) ? aid : null; }
  if (uniq.length === 2) return resolveCombo(TAKE_TWO_DIFF_COMBOS, TAKE_TWO_DIFF, uniq, legalSet);
  if (uniq.length === 3) return resolveCombo(TAKE_THREE_COMBOS, TAKE_THREE, uniq, legalSet);
  return null;
}

function resolveCombo(combos, offset, colors, legalSet) {
  const sorted = colors.slice().sort((a, b) => a - b);
  for (let i = 0; i < combos.length; i++) {
    const combo = combos[i];
    if (combo.length !== sorted.length) continue;
    if (combo.every((v, j) => v === sorted[j])) {
      const aid = offset + i;
      return legalSet.has(aid) ? aid : null;
    }
  }
  return null;
}

function hasAnyTakeAction(legalSet) {
  for (let a = TAKE_THREE; a < CHOOSE_NOBLE; a++) {
    if (legalSet.has(a)) return true;
  }
  return false;
}

function hasTakeExtension(colors, legalSet) {
  if (!colors || colors.length === 0 || colors.length > 3) return false;
  if (resolveTakeActionId(colors, legalSet) != null) return true;
  if (colors.length === 1) {
    const first = colors[0];
    if (resolveTakeActionId([first, first], legalSet) != null) return true;
    for (let c = 0; c < 5; c++) {
      if (c === first) continue;
      if (resolveTakeActionId([first, c], legalSet) != null) return true;
      for (let d = c + 1; d < 5; d++) {
        if (d === first) continue;
        if (resolveTakeActionId([first, c, d], legalSet) != null) return true;
      }
    }
    return false;
  }
  if (colors.length === 2 && colors[0] !== colors[1]) {
    for (let c = 0; c < 5; c++) {
      if (colors.includes(c)) continue;
      if (resolveTakeActionId([colors[0], colors[1], c], legalSet) != null) return true;
    }
    return false;
  }
  return false;
}

function canStartWithColor(c, legalSet) {
  return resolveTakeActionId([c], legalSet) != null || hasTakeExtension([c], legalSet);
}

function canAppendColor(nextColor, legalSet) {
  if (gemPick.length >= 3) return false;
  if (gemPick.length === 0) return canStartWithColor(nextColor, legalSet);
  const next = gemPick.concat([nextColor]);
  const uniqSet = new Set(next);
  if (uniqSet.size !== next.length && !(next.length === 2 && next[0] === next[1])) return false;
  return hasTakeExtension(next, legalSet);
}

// --- Rendering helpers ---

function tokenChip(colorIdx, count, options) {
  options = options || {};
  const el = document.createElement('button');
  el.type = 'button';
  el.className = 'token-chip ' + COLOR_NAMES[colorIdx];
  if (options.selected) el.classList.add('selected');
  el.textContent = COLOR_LABELS[colorIdx] + ':' + count;
  if (options.onClick && !options.disabled) {
    el.classList.add('clickable');
    el.addEventListener('click', options.onClick);
  } else {
    el.disabled = true;
  }
  return el;
}

function renderDevCard(card, actions) {
  const root = document.createElement('div');
  root.className = 'dev-card';
  if (!card) { root.classList.add('dev-card-placeholder'); return root; }

  const points = card.points || 0;
  const bonus = card.bonus != null ? card.bonus : 0;
  const tier = card.tier || 0;

  const head = document.createElement('div');
  head.className = 'dev-card-head';
  const left = document.createElement('span');
  left.className = 'dev-card-head-left';
  left.innerHTML = '<span>T' + tier + '</span><span class="bonus-inline ' + COLOR_NAMES[Math.max(0, Math.min(4, bonus))] + '">' + COLOR_LABELS[Math.max(0, Math.min(4, bonus))] + '</span>';
  const right = document.createElement('span');
  right.textContent = points + '分';
  head.append(left, right);
  root.appendChild(head);

  const costRow = document.createElement('div');
  costRow.className = 'cost-row';
  const cost = card.cost || [];
  for (let i = 0; i < 5; i++) {
    const v = cost[i] || 0;
    if (v <= 0) continue;
    const pill = document.createElement('span');
    pill.className = 'cost-gem ' + COLOR_NAMES[i];
    pill.textContent = String(v);
    costRow.appendChild(pill);
  }
  root.appendChild(costRow);

  actions = actions || [];
  if (actions.length > 0) {
    if (actions.some(a => !a.disabled && a.onClick)) root.classList.add('clickable');
    const actionRow = document.createElement('div');
    actionRow.className = 'card-op-row';
    for (const act of actions) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'card-op-btn';
      btn.textContent = act.label || '操作';
      btn.disabled = !!act.disabled;
      if (!btn.disabled && act.onClick) {
        btn.addEventListener('click', (ev) => { ev.stopPropagation(); act.onClick(); });
      }
      actionRow.appendChild(btn);
    }
    root.appendChild(actionRow);
  }
  return root;
}

// --- Main board render ---

function renderEmptyBoard(container) {
  const wrap = document.createElement('div');
  wrap.className = 'splendor-board';

  const bankSection = document.createElement('div');
  bankSection.className = 'bank-area';
  bankSection.innerHTML = '<h3>银行宝石</h3>';
  const bankRow = document.createElement('div');
  bankRow.className = 'bank-row';
  for (let c = 0; c < 6; c++) bankRow.appendChild(tokenChip(c, 0, {}));
  bankSection.appendChild(bankRow);
  wrap.appendChild(bankSection);

  const tableau = document.createElement('div');
  tableau.className = 'tableau-wrap';
  tableau.innerHTML = '<h3>发展卡</h3>';
  const grid = document.createElement('div');
  grid.className = 'tableau-grid-layout';
  for (let tier = 2; tier >= 0; tier--) {
    const line = document.createElement('div');
    line.className = 'tableau-tier-line';
    const cardRow = document.createElement('div');
    cardRow.className = 'card-row';
    for (let i = 0; i < 4; i++) cardRow.appendChild(renderDevCard(null));
    line.appendChild(cardRow);
    grid.appendChild(line);
  }
  tableau.appendChild(grid);
  wrap.appendChild(tableau);

  const nobles = document.createElement('div');
  nobles.className = 'nobles-area';
  nobles.innerHTML = '<h3>贵族</h3><div class="nobles-row"></div>';
  wrap.appendChild(nobles);

  container.appendChild(wrap);
}

function renderBoard(container, gameState, ctx) {
  currentCtx = ctx;
  container.innerHTML = '';

  if (!gameState || !gameState.state) {
    renderEmptyBoard(container);
    return;
  }

  const game = gameState.state;
  const legalSet = getLegalSet(gameState);
  const playing = ctx.canPlay;
  const stage = getStage(gameState);
  const inReturn = stage === 1;
  const inNoble = stage === 2;

  if (!playing || inReturn || inNoble) gemPick = [];

  const wrap = document.createElement('div');
  wrap.className = 'splendor-board';

  // Turn badge
  const turnBadge = document.createElement('div');
  turnBadge.className = 'turn-badge';
  const plies = game.plies || 0;
  turnBadge.textContent = '第' + (Math.floor(plies / 2) + 1) + '回合';

  // Bank
  const bankSection = document.createElement('div');
  bankSection.className = 'bank-area';
  const bankHead = document.createElement('div');
  bankHead.className = 'bank-head-line';
  bankHead.innerHTML = '<h3>银行宝石</h3>';
  bankHead.appendChild(turnBadge);
  bankSection.appendChild(bankHead);

  const bankRow = document.createElement('div');
  bankRow.className = 'bank-row';
  const bank = game.bank || [0, 0, 0, 0, 0, 0];
  for (let c = 0; c < 6; c++) {
    const selectable = c < 5 && playing && !inNoble &&
      (inReturn ? legalSet.has(RETURN_TOKEN + c) : canStartWithColor(c, legalSet));
    const selected = gemPick.includes(c);
    const chip = tokenChip(c, bank[c], {
      onClick: selectable ? (() => { const cc = c; return () => onBankGemClick(cc, gameState, ctx); })() : null,
      selected,
    });
    chip.dataset.bankGem = c;
    bankRow.appendChild(chip);
  }
  bankSection.appendChild(bankRow);

  const pickStatus = document.createElement('div');
  pickStatus.className = 'muted inline-title';
  if (inReturn) pickStatus.textContent = '返还模式：点击宝石堆返还 1 枚。';
  else if (inNoble) pickStatus.textContent = '贵族选择模式：点击可选贵族完成本回合。';
  else if (!playing) pickStatus.textContent = '当前不是你的回合。';
  else if (!hasAnyTakeAction(legalSet)) pickStatus.textContent = '当前无法拿宝石。';
  else if (gemPick.length === 0) pickStatus.textContent = '操作：点击宝石堆选择，三色点三次/同色点两次，然后点确认。';
  else pickStatus.textContent = '已选择：' + gemPick.map(c => COLOR_LABELS[c]).join('+') + '（可确认或取消）';
  bankSection.appendChild(pickStatus);

  const opsRow = document.createElement('div');
  opsRow.className = 'inline-ops';
  const takeAid = resolveTakeActionId(gemPick, legalSet);

  const confirmBtn = document.createElement('button');
  confirmBtn.type = 'button';
  confirmBtn.className = 'inline-op-btn';
  confirmBtn.textContent = '确认拿宝石';
  confirmBtn.disabled = !(playing && !inReturn && !inNoble && takeAid != null);
  confirmBtn.addEventListener('click', () => { if (takeAid != null) ctx.submitAction(takeAid); });

  const cancelBtn = document.createElement('button');
  cancelBtn.type = 'button';
  cancelBtn.className = 'inline-op-btn';
  cancelBtn.textContent = '取消选择';
  cancelBtn.disabled = gemPick.length === 0 || inNoble;
  cancelBtn.addEventListener('click', () => { gemPick = []; ctx.rerender(); });

  const passBtn = document.createElement('button');
  passBtn.type = 'button';
  passBtn.className = 'inline-op-btn';
  passBtn.textContent = '跳过回合';
  passBtn.disabled = !(playing && legalSet.has(PASS_ACTION) && gemPick.length === 0);
  passBtn.addEventListener('click', () => ctx.submitAction(PASS_ACTION));

  opsRow.append(confirmBtn, cancelBtn, passBtn);
  bankSection.appendChild(opsRow);

  const pendingMsg = document.createElement('div');
  pendingMsg.className = 'muted';
  if (inReturn) pendingMsg.textContent = '当前需返还 ' + (game.pending_returns || 0) + ' 枚宝石';
  else if (inNoble) pendingMsg.textContent = '当前需要选择一位贵族。';
  bankSection.appendChild(pendingMsg);

  wrap.appendChild(bankSection);

  // Nobles
  const noblesArea = document.createElement('div');
  noblesArea.className = 'nobles-area';
  noblesArea.innerHTML = '<h3>贵族</h3>';
  const noblesRow = document.createElement('div');
  noblesRow.className = 'nobles-row';
  const nobles = game.nobles || [];
  for (let ni = 0; ni < nobles.length; ni++) {
    const noble = nobles[ni];
    const card = document.createElement('div');
    card.className = 'noble-card';
    card.dataset.noble = ni;
    const selectable = playing && inNoble && legalSet.has(CHOOSE_NOBLE + ni);
    if (selectable) {
      card.classList.add('clickable');
      card.addEventListener('click', ((slot) => () => ctx.submitAction(CHOOSE_NOBLE + slot))(ni));
    }
    card.innerHTML = '<div class="noble-title">贵族 3分</div>';
    const reqRow = document.createElement('div');
    reqRow.className = 'cost-row';
    const req = noble.requirements || [];
    for (let c = 0; c < 5; c++) {
      const v = req[c] || 0;
      if (v <= 0) continue;
      const pill = document.createElement('span');
      pill.className = 'cost-gem ' + COLOR_NAMES[c];
      pill.textContent = String(v);
      reqRow.appendChild(pill);
    }
    card.appendChild(reqRow);
    if (selectable) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'card-op-btn';
      btn.textContent = '选择贵族';
      btn.addEventListener('click', (ev) => { ev.stopPropagation(); ctx.submitAction(CHOOSE_NOBLE + ni); });
      card.appendChild(btn);
    }
    noblesRow.appendChild(card);
  }
  noblesArea.appendChild(noblesRow);
  wrap.appendChild(noblesArea);

  // Tableau
  const tableauWrap = document.createElement('div');
  tableauWrap.className = 'tableau-wrap';
  tableauWrap.innerHTML = '<h3>桌面开发卡（T3/T2/T1）</h3>';
  const tableauLayout = document.createElement('div');
  tableauLayout.className = 'tableau-grid-layout';
  const tableau = game.tableau || [[], [], []];
  const deckSizes = game.deck_sizes || [0, 0, 0];

  for (let t = 2; t >= 0; t--) {
    const line = document.createElement('div');
    line.className = 'tableau-tier-line';

    const reserveDeckAid = RESERVE_DECK + t;
    const deckBtn = document.createElement('button');
    deckBtn.type = 'button';
    deckBtn.className = 'deck-op-btn deck-op-btn-left';
    deckBtn.textContent = '保留牌堆 (' + deckSizes[t] + ')';
    deckBtn.disabled = !(playing && legalSet.has(reserveDeckAid));
    deckBtn.dataset.deck = t;
    deckBtn.addEventListener('click', () => ctx.submitAction(reserveDeckAid));
    line.appendChild(deckBtn);

    const tier = document.createElement('div');
    tier.className = 'tableau-tier';
    tier.innerHTML = '<div class="tableau-tier-title">Tier ' + (t + 1) + '</div>';
    const row = document.createElement('div');
    row.className = 'card-row';
    const cards = tableau[t] || [];
    for (let s = 0; s < cards.length; s++) {
      const buyAid = BUY_FACEUP + t * 4 + s;
      const resAid = RESERVE_FACEUP + t * 4 + s;
      const buyOk = playing && legalSet.has(buyAid);
      const resOk = playing && legalSet.has(resAid);
      const cardEl = renderDevCard(cards[s], [
        { label: '购买', disabled: !buyOk, onClick: buyOk ? () => ctx.submitAction(buyAid) : null },
        { label: '保留', disabled: !resOk, onClick: resOk ? () => ctx.submitAction(resAid) : null },
      ]);
      cardEl.dataset.tableau = t + '-' + s;
      row.appendChild(cardEl);
    }
    while (row.children.length < 4) row.appendChild(renderDevCard(null));
    tier.appendChild(row);
    line.appendChild(tier);
    tableauLayout.appendChild(line);
  }
  tableauWrap.appendChild(tableauLayout);
  wrap.appendChild(tableauWrap);

  container.appendChild(wrap);
}

function onBankGemClick(colorIdx, gameState, ctx) {
  const legalSet = getLegalSet(gameState);
  const stage = getStage(gameState);
  if (stage === 1) {
    const returnAid = RETURN_TOKEN + colorIdx;
    if (legalSet.has(returnAid)) ctx.submitAction(returnAid);
    return;
  }
  if (stage === 2) return;
  if (!hasAnyTakeAction(legalSet)) return;
  if (!canAppendColor(colorIdx, legalSet)) return;
  gemPick = gemPick.concat([colorIdx]);
  ctx.rerender();
}

// --- Player area ---

function renderPlayerArea(container, gameState, ctx) {
  container.innerHTML = '';
  if (!gameState || !gameState.state) {
    const outer = document.createElement('div');
    outer.className = 'splendor-players-wrap';
    const wrap = document.createElement('div');
    wrap.className = 'players-wrap';
    for (let i = 0; i < 2; i++) {
      const card = document.createElement('div');
      card.className = 'player-card';
      card.innerHTML = '<div class="player-title"><span>玩家' + i + '</span></div>';
      wrap.appendChild(card);
    }
    outer.appendChild(wrap);
    container.appendChild(outer);
    return;
  }

  const game = gameState.state;
  const appState = ctx.state;
  const legalSet = getLegalSet(gameState);
  const playing = ctx.canPlay;
  const players = game.players || [];
  const cp = gameState.current_player;

  const outer = document.createElement('div');
  outer.className = 'splendor-players-wrap';

  const wrap = document.createElement('div');
  wrap.className = 'players-wrap';

  const numPlayers = gameState.num_players || 2;
  if (numPlayers > 2) wrap.classList.add('players-multi');

  for (let p = 0; p < numPlayers; p++) {
    const pd = players[p] || {};
    const isHuman = p === appState.humanPlayer;
    const isCurrent = p === cp;

    const card = document.createElement('div');
    card.className = 'player-card' + (isCurrent ? ' active-turn' : '');
    card.dataset.player = p;

    const title = document.createElement('div');
    title.className = 'player-title';
    title.innerHTML = '<span>玩家' + p + (isHuman ? '（你）' : '') + (isCurrent ? ' · 当前' : '') + '</span>' +
      '<span class="player-title-meta">分:' + (pd.points || 0) + '  卡:' + (pd.cards_count || 0) + '  贵族:' + (pd.nobles_count || 0) + '</span>';
    card.appendChild(title);

    // Gems
    const gemsGroup = document.createElement('div');
    gemsGroup.className = 'player-stat-group';
    const gems = pd.gems || [];
    const gemsTotal = gems.reduce((s, v) => s + (v || 0), 0);
    gemsGroup.innerHTML = '<div class="player-stat-label">宝石 <span class="player-stat-extra">总数:' + gemsTotal + '</span></div>';
    const gemsRow = document.createElement('div');
    gemsRow.className = 'token-row';
    for (let c = 0; c < 6; c++) {
      const chip = tokenChip(c, gems[c] || 0);
      chip.dataset.playerGem = p + '-' + c;
      gemsRow.appendChild(chip);
    }
    gemsGroup.appendChild(gemsRow);
    card.appendChild(gemsGroup);

    // Bonuses
    const bonusGroup = document.createElement('div');
    bonusGroup.className = 'player-stat-group';
    bonusGroup.innerHTML = '<div class="player-stat-label">发展卡</div>';
    const bonusRow = document.createElement('div');
    bonusRow.className = 'bonus-row';
    const bonuses = pd.bonuses || [];
    for (let c = 0; c < 5; c++) {
      const chip = document.createElement('span');
      chip.className = 'bonus-rect ' + COLOR_NAMES[c];
      chip.textContent = COLOR_LABELS[c] + ':' + (bonuses[c] || 0);
      bonusRow.appendChild(chip);
    }
    bonusGroup.appendChild(bonusRow);
    card.appendChild(bonusGroup);

    // Reserved — always render 3 fixed slots so the card height doesn't jump
    // when the player reserves a card mid-game.
    const reserveGroup = document.createElement('div');
    reserveGroup.className = 'player-stat-group reserve-group';
    reserveGroup.innerHTML = '<div class="player-stat-label">保留卡 (最多 3 张)</div>';
    const reserveRow = document.createElement('div');
    reserveRow.className = 'reserve-row';
    const reserved = pd.reserved || [];
    for (let ri = 0; ri < 3; ri++) {
      const item = reserved[ri];
      if (!item) {
        const empty = document.createElement('div');
        empty.className = 'reserve-slot-empty';
        empty.dataset.reserved = p + '-' + ri;
        empty.textContent = '空';
        reserveRow.appendChild(empty);
        continue;
      }
      if (!item.visible && !isHuman) {
        const hidden = document.createElement('div');
        hidden.className = 'reserve-hidden';
        hidden.dataset.reserved = p + '-' + ri;
        hidden.textContent = '暗保留 T' + (item.tier || '?');
        reserveRow.appendChild(hidden);
        continue;
      }
      const buyResAid = BUY_RESERVED + ri;
      const canBuy = isHuman && playing && legalSet.has(buyResAid);
      const actions = canBuy ? [{ label: '购买保留', onClick: () => ctx.submitAction(buyResAid) }] : [];
      const resCardEl = renderDevCard(item, actions);
      resCardEl.dataset.reserved = p + '-' + ri;
      reserveRow.appendChild(resCardEl);
    }
    reserveGroup.appendChild(reserveRow);
    card.appendChild(reserveGroup);

    wrap.appendChild(card);
  }

  outer.appendChild(wrap);
  container.appendChild(outer);
}

function formatMove(actionInfo, actionId) {
  if (!actionInfo && (actionId === null || actionId === undefined)) return '开局';
  return formatActionInfo(actionInfo, actionId);
}

// --- Animation ---

function makeFlyingToken(colorIdx) {
  const el = document.createElement('div');
  el.className = 'anim-flying-token ' + COLOR_NAMES[colorIdx];
  el.textContent = COLOR_LABELS[colorIdx];
  return el;
}

// Clone a live card DOM element (dev-card / noble-card) so the flyer
// shows the full card visual instead of a blank rectangle. Strips
// interactive affordances (buttons, hover hooks) so the cloned sprite
// is purely decorative mid-flight.
function cloneCardEl(selector) {
  const src = document.querySelector(selector);
  if (!src) return null;
  const clone = src.cloneNode(true);
  clone.querySelectorAll('button').forEach(b => b.remove());
  clone.classList.remove('clickable');
  clone.style.pointerEvents = 'none';
  clone.style.margin = '0';
  return clone;
}

const CARD_FLY_MS = 550;
const TOKEN_FLY_MS = 400;

function makeHiddenCardFlyer(tier) {
  const el = document.createElement('div');
  el.className = 'dev-card dev-card-hidden';
  el.textContent = 'T' + ((tier || 0) + 1);
  return el;
}

// After a reserve fly finishes, the destination slot in the DOM is still
// the "empty" placeholder — and the flyer sprite is removed by animate.js.
// If a *subsequent* animation step plays (e.g. the deck→tableau refill),
// the user sees a gap where the just-reserved card should be. Paint the
// slot into a persistent static representation so the intermediate state
// survives until the caller re-renders the real DOM. See §3 "中间状态维护"
// in docs/WEB_DESIGN_PRINCIPLES.md.
function paintReserveSlotFromCard(selector, sourceCardSelector) {
  const slot = document.querySelector(selector);
  if (!slot) return;
  const src = document.querySelector(sourceCardSelector);
  if (!src) return;
  // Replace the empty placeholder class so the slot renders as a card
  // (.reserve-slot-empty has dashed borders + "空" text that would show
  // through otherwise). Copy the source card's innerHTML — contents are
  // the visible card face — and strip interactive affordances.
  slot.className = 'dev-card';
  slot.innerHTML = src.innerHTML;
  slot.querySelectorAll('button').forEach(b => b.remove());
  slot.classList.remove('clickable');
  slot.style.pointerEvents = 'none';
}

function paintReserveSlotHidden(selector, tier) {
  const slot = document.querySelector(selector);
  if (!slot) return;
  slot.className = 'dev-card dev-card-hidden';
  slot.textContent = 'T' + ((tier || 0) + 1);
}

function refillStepsFromDeck(prev, next) {
  // When a tableau slot is taken (buy_faceup / reserve_faceup), the engine
  // deals a replacement from the deck. Add a deck→slot fly for each tier
  // whose deck_sizes dropped by 1 and whose tableau got a new card.
  const refills = [];
  const prevDecks = prev.deck_sizes || [];
  const nextDecks = next.deck_sizes || [];
  for (let t = 0; t < 3; t++) {
    const drew = (prevDecks[t] || 0) - (nextDecks[t] || 0);
    if (drew <= 0) continue;
    const prevRow = (prev.tableau && prev.tableau[t]) || [];
    const nextRow = (next.tableau && next.tableau[t]) || [];
    // Tier's refill fills whichever slot position ended up with a new card;
    // we animate by slot index — if a specific slot was taken, the deck
    // refills that position.
    const len = Math.max(prevRow.length, nextRow.length);
    for (let s = 0; s < len; s++) {
      const pc = prevRow[s];
      const nc = nextRow[s];
      const changed = !pc || !nc || pc.id !== nc.id || pc.bonus !== nc.bonus || pc.points !== nc.points;
      if (!changed) continue;
      refills.push({
        from: `[data-deck="${t}"]`,
        to: `[data-tableau="${t}-${s}"]`,
        createElement: () => makeHiddenCardFlyer(t),
        duration: CARD_FLY_MS,
      });
      break; // one refill per draw
    }
  }
  return refills;
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !actionInfo.type) return null;
  if (!prevState || !prevState.state || !newState || !newState.state) return null;

  const steps = [];
  const actor = prevState.current_player;
  const prev = prevState.state;
  const next = newState.state;

  switch (actionInfo.type) {
    case 'take_three':
    case 'take_two_different': {
      const colors = actionInfo.colors || [];
      const flights = colors.map(c => ({
        from: `[data-bank-gem="${c}"]`,
        to: `[data-player-gem="${actor}-${c}"]`,
        createElement: () => makeFlyingToken(c),
        duration: TOKEN_FLY_MS,
      }));
      if (flights.length) steps.push({ type: 'flyGroup', flights });
      break;
    }

    case 'take_one': {
      const c = actionInfo.color;
      if (c != null) {
        steps.push({
          type: 'fly',
          from: `[data-bank-gem="${c}"]`,
          to: `[data-player-gem="${actor}-${c}"]`,
          createElement: () => makeFlyingToken(c),
          duration: TOKEN_FLY_MS,
        });
      }
      break;
    }

    case 'take_two_same': {
      const c = actionInfo.color;
      if (c != null) {
        const flights = [0, 1].map(() => ({
          from: `[data-bank-gem="${c}"]`,
          to: `[data-player-gem="${actor}-${c}"]`,
          createElement: () => makeFlyingToken(c),
          duration: TOKEN_FLY_MS,
        }));
        steps.push({ type: 'flyGroup', flights });
      }
      break;
    }

    case 'buy_faceup': {
      const gemFlights = gemReturnFlights(prev, next, actor);
      const fromSel = `[data-tableau="${actionInfo.tier}-${actionInfo.slot}"]`;
      const cardFlight = {
        from: fromSel,
        to: `[data-player="${actor}"] .bonus-row`,
        createElement: () => cloneCardEl(fromSel),
        hideFrom: true,
        duration: CARD_FLY_MS,
      };
      steps.push({ type: 'flyGroup', flights: [...gemFlights, cardFlight] });
      // After the taken slot is hidden, fly a replacement from the deck
      // into the same slot so players see the refill explicitly.
      const refills = refillStepsFromDeck(prev, next);
      if (refills.length) steps.push({ type: 'flyGroup', flights: refills });
      break;
    }

    case 'buy_reserved': {
      const gemFlights = gemReturnFlights(prev, next, actor);
      const fromSel = `[data-reserved="${actor}-${actionInfo.slot}"]`;
      const cardFlight = {
        from: fromSel,
        to: `[data-player="${actor}"] .bonus-row`,
        createElement: () => cloneCardEl(fromSel),
        hideFrom: true,
        duration: CARD_FLY_MS,
      };
      steps.push({ type: 'flyGroup', flights: [...gemFlights, cardFlight] });
      break;
    }

    case 'reserve_faceup': {
      // Find the empty reserve slot that will receive the card in the
      // next state. Reserved cards are appended, so the target slot
      // index is the count before reserving.
      const prevReserved = (prev.players[actor].reserved || []).length;
      const fromSel = `[data-tableau="${actionInfo.tier}-${actionInfo.slot}"]`;
      const toSel = `[data-player="${actor}"] [data-reserved="${actor}-${prevReserved}"]`;
      // Keep a pre-flight clone around so that after the tableau source is
      // hidden we can still reconstruct the reserved card into the empty
      // slot during onComplete (prevents the "reserved card disappears
      // mid-deck-refill" bug — see §3 中间状态维护 in web-design-principles).
      const preservedCardSel = fromSel;
      const cardFlight = {
        from: fromSel,
        to: toSel,
        createElement: () => cloneCardEl(fromSel),
        hideFrom: true,
        duration: CARD_FLY_MS,
        onComplete: () => paintReserveSlotFromCard(toSel, preservedCardSel),
      };
      const flights = [cardFlight];
      const prevGold = (prev.players[actor].gems || [])[5] || 0;
      const nextGold = (next.players[actor].gems || [])[5] || 0;
      if (nextGold > prevGold) {
        flights.push({
          from: '[data-bank-gem="5"]',
          to: `[data-player-gem="${actor}-5"]`,
          createElement: () => makeFlyingToken(5),
          duration: TOKEN_FLY_MS,
        });
      }
      steps.push({ type: 'flyGroup', flights });
      // Refill the vacated tableau slot from the deck.
      const refills = refillStepsFromDeck(prev, next);
      if (refills.length) steps.push({ type: 'flyGroup', flights: refills });
      break;
    }

    case 'reserve_deck': {
      const prevReserved = (prev.players[actor].reserved || []).length;
      const toSel = `[data-player="${actor}"] [data-reserved="${actor}-${prevReserved}"]`;
      const tier = actionInfo.tier;
      const flights = [{
        from: `[data-deck="${actionInfo.tier}"]`,
        to: toSel,
        createElement: () => makeHiddenCardFlyer(tier),
        duration: CARD_FLY_MS,
        // Keep a hidden-card placeholder in the reserve slot so later
        // flights (e.g. gold coin from the bank) don't leave a visual gap
        // where the reserved card should be. Opponents' reserves from the
        // deck are rendered as `.reserve-hidden` in steady state, but for
        // the intermediate animation state using the same hidden-card
        // visual is fine — re-render replaces it anyway.
        onComplete: () => paintReserveSlotHidden(toSel, tier),
      }];
      const prevGold = (prev.players[actor].gems || [])[5] || 0;
      const nextGold = (next.players[actor].gems || [])[5] || 0;
      if (nextGold > prevGold) {
        flights.push({
          from: '[data-bank-gem="5"]',
          to: `[data-player-gem="${actor}-5"]`,
          createElement: () => makeFlyingToken(5),
          duration: TOKEN_FLY_MS,
        });
      }
      steps.push({ type: 'flyGroup', flights });
      break;
    }

    case 'choose_noble': {
      const fromSel = `[data-noble="${actionInfo.noble_slot}"]`;
      steps.push({
        type: 'fly',
        from: fromSel,
        to: `[data-player="${actor}"]`,
        createElement: () => cloneCardEl(fromSel),
        hideFrom: true,
        duration: CARD_FLY_MS,
      });
      break;
    }

    case 'return_token': {
      const c = actionInfo.token;
      if (c != null) {
        steps.push({
          type: 'fly',
          from: `[data-player-gem="${actor}-${c}"]`,
          to: `[data-bank-gem="${c}"]`,
          createElement: () => makeFlyingToken(c),
          duration: TOKEN_FLY_MS,
        });
      }
      break;
    }
  }

  return steps.length ? steps : null;
}

function gemReturnFlights(prev, next, actor) {
  const flights = [];
  const prevGems = prev.players[actor].gems || [];
  const nextGems = next.players[actor].gems || [];
  for (let c = 0; c < 6; c++) {
    const spent = (prevGems[c] || 0) - (nextGems[c] || 0);
    for (let i = 0; i < spent; i++) {
      flights.push({
        from: `[data-player-gem="${actor}-${c}"]`,
        to: `[data-bank-gem="${c}"]`,
        createElement: () => makeFlyingToken(c),
        duration: TOKEN_FLY_MS,
      });
    }
  }
  return flights;
}

function gemReturnSteps(steps, prev, next, actor) {
  const prevGems = prev.players[actor].gems || [];
  const nextGems = next.players[actor].gems || [];
  for (let c = 0; c < 6; c++) {
    const spent = (prevGems[c] || 0) - (nextGems[c] || 0);
    if (spent > 0) {
      steps.push({
        type: 'fly',
        from: `[data-player-gem="${actor}-${c}"]`,
        to: `[data-bank-gem="${c}"]`,
        createElement: () => makeFlyingToken(c),
        duration: TOKEN_FLY_MS,
      });
    }
  }
}

createApp({
  gameId: 'splendor',
  gameTitle: 'Splendor',
  gameIntro: '拿宝石、买卡、抢贵族，先到15分触发终局',
  players: { min: 2, max: 4 },
  renderBoard,
  renderPlayerArea,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  getPlayerSymbol: (humanPlayer) => '玩家' + humanPlayer,
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
});
