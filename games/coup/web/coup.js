import { createApp } from '/static/general/app.js';

const CHAR_NAMES = ['Duke', 'Assassin', 'Captain', 'Ambassador', 'Contessa'];
const CHAR_LABELS = ['公爵', '刺客', '队长', '大使', '伯爵夫人'];
const CHAR_COLORS = ['#c8a', '#e55', '#58c', '#5a8', '#ea8'];

const STAGE_NAMES = [
  '声明行动', '质疑行动', '解决质疑', '失去影响力',
  '反制', '质疑反制', '解决反制质疑', '失去影响力(反制)',
  '交换还牌1', '交换还牌2', '失去影响力(行动)',
];

const INCOME = 0, FOREIGN_AID = 1;
const COUP_OFF = 2, COUP_COUNT = 4;
const TAX = 6;
const ASSASSINATE_OFF = 7, ASSASSINATE_COUNT = 4;
const STEAL_OFF = 11, STEAL_COUNT = 4;
const EXCHANGE = 15;
const CHALLENGE = 16, ALLOW = 17;
const BLOCK_DUKE = 18, BLOCK_CONTESSA = 19, BLOCK_AMBASSADOR = 20, BLOCK_CAPTAIN = 21;
const ALLOW_NO_BLOCK = 22;
const REVEAL_0 = 23, REVEAL_1 = 24;
const LOSE_0 = 25, LOSE_1 = 26;
const RETURN_DUKE = 27;

let pendingActionType = null;
let currentCtx = null;

function getLegalSet(gs) {
  if (!gs) return new Set();
  return new Set(gs.legal_actions || []);
}

function resetPending() {
  pendingActionType = null;
}

function renderBoard(container, gs, ctx) {
  currentCtx = ctx;
  container.innerHTML = '';
  if (!gs || !gs.state) return;

  const st = gs.state;
  const legalSet = getLegalSet(gs);
  const playing = ctx.canPlay;
  const humanPlayer = ctx.state.humanPlayer;
  const numPlayers = st.num_players;
  const stage = st.stage;

  const board = document.createElement('div');
  board.className = 'coup-board';

  const info = document.createElement('div');
  info.className = 'coup-info-bar';
  info.innerHTML = '<span class="coup-stage">' + STAGE_NAMES[stage] + '</span>'
    + '<span class="coup-deck">牌堆: ' + st.deck_size + '</span>';
  board.appendChild(info);

  const playersArea = document.createElement('div');
  playersArea.className = 'coup-players';

  for (let pi = 0; pi < numPlayers; pi++) {
    if (pi === humanPlayer) continue;
    const p = st.players[pi];
    const el = document.createElement('div');
    el.className = 'coup-player-card';
    el.setAttribute('data-player', String(pi));
    if (!p.alive) el.classList.add('eliminated');
    if (pi === st.current_player) el.classList.add('current-turn');
    if (pi === st.active_player) el.classList.add('active');

    const canTarget = playing && pendingActionType !== null;
    let isValidTarget = false;
    if (canTarget) {
      let actionId = -1;
      if (pendingActionType === 'coup') actionId = COUP_OFF + pi;
      else if (pendingActionType === 'assassinate') actionId = ASSASSINATE_OFF + pi;
      else if (pendingActionType === 'steal') actionId = STEAL_OFF + pi;
      if (actionId >= 0 && legalSet.has(actionId)) isValidTarget = true;
    }

    if (isValidTarget) {
      el.classList.add('selectable');
      el.addEventListener('click', () => {
        let aid;
        if (pendingActionType === 'coup') aid = COUP_OFF + pi;
        else if (pendingActionType === 'assassinate') aid = ASSASSINATE_OFF + pi;
        else if (pendingActionType === 'steal') aid = STEAL_OFF + pi;
        resetPending();
        ctx.submitAction(aid);
      });
    }

    const nameRow = document.createElement('div');
    nameRow.className = 'coup-pname';
    nameRow.textContent = '玩家' + pi;
    if (!p.alive) nameRow.textContent += ' (淘汰)';
    el.appendChild(nameRow);

    const coinsEl = document.createElement('div');
    coinsEl.className = 'coup-coins';
    coinsEl.setAttribute('data-coins', String(pi));
    coinsEl.textContent = '💰 ' + p.coins;
    el.appendChild(coinsEl);

    const infArea = document.createElement('div');
    infArea.className = 'coup-influences';
    for (let sl = 0; sl < 2; sl++) {
      const inf = p.influences[sl];
      const card = document.createElement('div');
      card.className = 'coup-inf-card';
      card.setAttribute('data-influence', pi + '-' + sl);
      if (inf.revealed) {
        card.classList.add('revealed');
        card.textContent = CHAR_LABELS[inf.character];
        card.style.borderColor = CHAR_COLORS[inf.character];
      } else {
        card.classList.add('hidden');
        card.textContent = '?';
      }
      infArea.appendChild(card);
    }
    el.appendChild(infArea);

    playersArea.appendChild(el);
  }
  board.appendChild(playersArea);

  if (playing && stage === 0) {
    board.appendChild(renderDeclareActions(st, legalSet, humanPlayer, ctx));
  } else if (playing && (stage === 1 || stage === 5)) {
    board.appendChild(renderChallengePanel(legalSet, ctx));
  } else if (playing && stage === 4) {
    board.appendChild(renderCounterPanel(st, legalSet, ctx));
  } else if (playing && (stage === 2 || stage === 6)) {
    board.appendChild(renderRevealPanel(st, legalSet, humanPlayer, ctx, '亮牌证明'));
  } else if (playing && (stage === 3 || stage === 7 || stage === 10)) {
    board.appendChild(renderRevealPanel(st, legalSet, humanPlayer, ctx, '选择失去影响力'));
  } else if (playing && (stage === 8 || stage === 9)) {
    board.appendChild(renderExchangePanel(st, legalSet, ctx));
  }

  container.appendChild(board);
}

function renderDeclareActions(st, legalSet, humanPlayer, ctx) {
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = '选择行动';
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';

  const actions = [
    { id: INCOME, label: '收入', desc: '+1💰', type: 'safe' },
    { id: FOREIGN_AID, label: '外援', desc: '+2💰', type: 'safe' },
    { id: TAX, label: '征税', desc: '+3💰 (公爵)', type: 'bluff' },
    { id: EXCHANGE, label: '交换', desc: '换牌 (大使)', type: 'bluff' },
  ];

  for (const a of actions) {
    if (!legalSet.has(a.id)) continue;
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn ' + a.type;
    btn.innerHTML = '<span class="coup-btn-label">' + a.label + '</span>'
      + '<span class="coup-btn-desc">' + a.desc + '</span>';
    btn.addEventListener('click', () => {
      resetPending();
      ctx.submitAction(a.id);
    });
    btns.appendChild(btn);
  }

  const targetActions = [
    { type: 'coup', label: '政变', desc: '-7💰 (不可阻挡)', off: COUP_OFF, count: COUP_COUNT, cls: 'coup-act' },
    { type: 'assassinate', label: '暗杀', desc: '-3💰 (刺客)', off: ASSASSINATE_OFF, count: ASSASSINATE_COUNT, cls: 'bluff' },
    { type: 'steal', label: '偷窃', desc: '偷2💰 (队长)', off: STEAL_OFF, count: STEAL_COUNT, cls: 'bluff' },
  ];

  for (const ta of targetActions) {
    let hasAny = false;
    for (let t = 0; t < 4; t++) {
      if (legalSet.has(ta.off + t)) { hasAny = true; break; }
    }
    if (!hasAny) continue;

    const btn = document.createElement('button');
    btn.className = 'coup-action-btn ' + ta.cls;
    if (pendingActionType === ta.type) btn.classList.add('selected');
    btn.innerHTML = '<span class="coup-btn-label">' + ta.label + '</span>'
      + '<span class="coup-btn-desc">' + ta.desc + '</span>';
    btn.addEventListener('click', () => {
      if (pendingActionType === ta.type) {
        resetPending();
        ctx.rerender();
      } else {
        pendingActionType = ta.type;
        ctx.rerender();
      }
    });
    btns.appendChild(btn);
  }

  panel.appendChild(btns);

  if (pendingActionType) {
    const hint = document.createElement('div');
    hint.className = 'coup-pending-hint';
    hint.textContent = '请点击目标玩家';
    panel.appendChild(hint);
  }

  return panel;
}

function renderChallengePanel(legalSet, ctx) {
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = '是否质疑？';
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';

  if (legalSet.has(CHALLENGE)) {
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn challenge';
    btn.textContent = '质疑!';
    btn.addEventListener('click', () => ctx.submitAction(CHALLENGE));
    btns.appendChild(btn);
  }
  if (legalSet.has(ALLOW)) {
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn allow';
    btn.textContent = '允许';
    btn.addEventListener('click', () => ctx.submitAction(ALLOW));
    btns.appendChild(btn);
  }

  panel.appendChild(btns);
  return panel;
}

function renderCounterPanel(st, legalSet, ctx) {
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = '是否反制？';
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';

  const blocks = [
    { id: BLOCK_DUKE, label: '反制 (公爵)', cls: 'block' },
    { id: BLOCK_CONTESSA, label: '反制 (伯爵夫人)', cls: 'block' },
    { id: BLOCK_AMBASSADOR, label: '反制 (大使)', cls: 'block' },
    { id: BLOCK_CAPTAIN, label: '反制 (队长)', cls: 'block' },
  ];

  for (const b of blocks) {
    if (!legalSet.has(b.id)) continue;
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn ' + b.cls;
    btn.textContent = b.label;
    btn.addEventListener('click', () => ctx.submitAction(b.id));
    btns.appendChild(btn);
  }

  if (legalSet.has(ALLOW_NO_BLOCK)) {
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn allow';
    btn.textContent = '不反制';
    btn.addEventListener('click', () => ctx.submitAction(ALLOW_NO_BLOCK));
    btns.appendChild(btn);
  }

  panel.appendChild(btns);
  return panel;
}

function renderRevealPanel(st, legalSet, humanPlayer, ctx, titleText) {
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = titleText;
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';

  const isReveal = legalSet.has(REVEAL_0) || legalSet.has(REVEAL_1);
  const p = st.players[humanPlayer];

  for (let sl = 0; sl < 2; sl++) {
    const revealId = isReveal ? (sl === 0 ? REVEAL_0 : REVEAL_1) : (sl === 0 ? LOSE_0 : LOSE_1);
    if (!legalSet.has(revealId)) continue;

    const inf = p.influences[sl];
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn card-choice';
    btn.style.borderColor = CHAR_COLORS[inf.character];
    btn.textContent = CHAR_LABELS[inf.character];
    btn.addEventListener('click', () => ctx.submitAction(revealId));
    btns.appendChild(btn);
  }

  panel.appendChild(btns);
  return panel;
}

function renderExchangePanel(st, legalSet, ctx) {
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = st.stage === 8 ? '选择还回第1张牌' : '选择还回第2张牌';
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';

  for (let c = 0; c < 5; c++) {
    const returnId = RETURN_DUKE + c;
    if (!legalSet.has(returnId)) continue;
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn card-choice';
    btn.style.borderColor = CHAR_COLORS[c];
    btn.textContent = '还 ' + CHAR_LABELS[c];
    btn.addEventListener('click', () => ctx.submitAction(returnId));
    btns.appendChild(btn);
  }

  panel.appendChild(btns);
  return panel;
}

function renderPlayerArea(container, gs, ctx) {
  container.innerHTML = '';
  if (!gs || !gs.state) return;

  const st = gs.state;
  const humanPlayer = ctx.state.humanPlayer;
  const p = st.players[humanPlayer];

  const area = document.createElement('div');
  area.className = 'coup-my-area';

  if (!p.alive) {
    const dead = document.createElement('div');
    dead.className = 'coup-dead-msg';
    dead.textContent = '你已被淘汰';
    area.appendChild(dead);
    container.appendChild(area);
    return;
  }

  const coinsEl = document.createElement('div');
  coinsEl.className = 'coup-my-coins';
  coinsEl.textContent = '💰 ' + p.coins;
  area.appendChild(coinsEl);

  const hand = document.createElement('div');
  hand.className = 'coup-my-hand';

  for (let sl = 0; sl < 2; sl++) {
    const inf = p.influences[sl];
    const card = document.createElement('div');
    card.className = 'coup-my-card';
    if (inf.revealed) {
      card.classList.add('revealed');
      card.textContent = CHAR_LABELS[inf.character];
      card.style.borderColor = '#555';
    } else {
      card.style.borderColor = CHAR_COLORS[inf.character];
      card.style.background = 'linear-gradient(135deg, ' + CHAR_COLORS[inf.character] + '22, #1e1e3a)';
      const name = document.createElement('div');
      name.className = 'coup-mycard-name';
      name.textContent = CHAR_LABELS[inf.character];
      card.appendChild(name);

      const charName = document.createElement('div');
      charName.className = 'coup-mycard-en';
      charName.textContent = CHAR_NAMES[inf.character];
      card.appendChild(charName);
    }
    hand.appendChild(card);
  }

  area.appendChild(hand);
  container.appendChild(area);
}

function formatMove(info, actionId) {
  if (!info || !info.type) {
    if (actionId === null || actionId === undefined) return '开局';
    return '动作 #' + actionId;
  }
  switch (info.type) {
    case 'income': return '收入 (+1💰)';
    case 'foreign_aid': return '外援 (+2💰)';
    case 'coup': return '政变 -> 玩家' + info.target;
    case 'tax': return '征税 [公爵] (+3💰)';
    case 'assassinate': return '暗杀 -> 玩家' + info.target + ' [刺客]';
    case 'steal': return '偷窃 -> 玩家' + info.target + ' [队长]';
    case 'exchange': return '交换 [大使]';
    case 'challenge': return '质疑!';
    case 'allow': return '允许';
    case 'block': return '反制 [' + (info.claimed || '') + ']';
    case 'allow_no_block': return '不反制';
    case 'reveal': return '亮牌 slot' + info.slot;
    case 'lose_influence': return '失去影响力 slot' + info.slot;
    case 'return_card': return '还 ' + (info.character_name || '');
    default: return '动作 #' + actionId;
  }
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const steps = [];
  const type = actionInfo.type;
  const actor = prevState.current_player;
  const target = actionInfo.target;

  if (type === 'coup' || type === 'assassinate') {
    if (target != null && target >= 0) {
      steps.push({
        type: 'highlight',
        target: '[data-player="' + target + '"]',
        className: 'anim-highlight',
        duration: 600,
      });
    }
  } else if (type === 'steal') {
    if (target != null && target >= 0) {
      steps.push({
        type: 'fly',
        from: '[data-coins="' + target + '"]',
        to: '[data-coins="' + actor + '"]',
        createElement() {
          const el = document.createElement('div');
          el.className = 'anim-flying-token';
          el.style.background = '#eab308';
          el.style.color = '#111';
          el.textContent = '💰';
          return el;
        },
        duration: 400,
      });
    }
  } else if (type === 'reveal' || type === 'lose_influence') {
    const slot = actionInfo.slot;
    if (slot != null) {
      steps.push({
        type: 'highlight',
        target: '[data-influence="' + actor + '-' + slot + '"]',
        className: 'anim-highlight',
        duration: 600,
      });
    }
  }

  return steps.length ? steps : null;
}

createApp({
  gameId: 'coup',
  gameTitle: 'Coup',
  gameIntro: '虚张声势、质疑与暗杀 — 影响力为零即淘汰',
  players: { min: 2, max: 4 },
  renderBoard,
  renderPlayerArea,
  describeTransition,
  formatOpponentMove: formatMove,
  formatSuggestedMove: formatMove,
  getPlayerSymbol: (p) => '玩家' + p,
  difficulties: ['heuristic', 'casual', 'expert'],
  defaultDifficulty: 'expert',
  onActionSubmitted: () => { resetPending(); },
  onGameStart: () => { resetPending(); },
  onUndo: () => { resetPending(); },
});
