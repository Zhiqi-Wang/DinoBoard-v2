import { createApp } from '/static/general/app.js';

const CARD_NAMES = ['', 'Guard', 'Priest', 'Baron', 'Handmaid', 'Prince', 'King', 'Countess', 'Princess'];
const CARD_LABELS = ['', '侍卫', '牧师', '男爵', '侍女', '王子', '国王', '伯爵夫人', '公主'];
const CARD_VALUES = ['', '1', '2', '3', '4', '5', '6', '7', '8'];
const CARD_EFFECTS = [
  '',
  '猜对手手牌',
  '偷看对手手牌',
  '比较手牌大小',
  '保护自己一轮',
  '迫使弃牌重摸',
  '交换手牌',
  '持有时必须弃出',
  '弃出即淘汰',
];

const GUARD_OFF = 0, PRIEST_OFF = 28, BARON_OFF = 32;
const HANDMAID_ACT = 36, PRINCE_OFF = 37, KING_OFF = 41;
const COUNTESS_ACT = 45, PRINCESS_ACT = 46;

let pendingCard = 0;
let pendingTarget = -1;
let currentCtx = null;

function getLegalSet(gs) {
  if (!gs) return new Set();
  return new Set(gs.legal_actions || []);
}

function cardOfAction(aid) {
  if (aid >= GUARD_OFF && aid < PRIEST_OFF) return 1;
  if (aid >= PRIEST_OFF && aid < BARON_OFF) return 2;
  if (aid >= BARON_OFF && aid < HANDMAID_ACT) return 3;
  if (aid === HANDMAID_ACT) return 4;
  if (aid >= PRINCE_OFF && aid < KING_OFF) return 5;
  if (aid >= KING_OFF && aid < COUNTESS_ACT) return 6;
  if (aid === COUNTESS_ACT) return 7;
  if (aid === PRINCESS_ACT) return 8;
  return 0;
}

function canPlayCard(card, legalSet) {
  for (const aid of legalSet) {
    if (cardOfAction(aid) === card) return true;
  }
  return false;
}

function needsTarget(card) {
  return card === 1 || card === 2 || card === 3 || card === 5 || card === 6;
}

function needsGuess(card) {
  return card === 1;
}

function getTargetsForCard(card, legalSet) {
  const targets = new Set();
  for (const aid of legalSet) {
    if (cardOfAction(aid) !== card) continue;
    let t = -1;
    if (card === 1) t = Math.floor((aid - GUARD_OFF) / 7);
    else if (card === 2) t = aid - PRIEST_OFF;
    else if (card === 3) t = aid - BARON_OFF;
    else if (card === 5) t = aid - PRINCE_OFF;
    else if (card === 6) t = aid - KING_OFF;
    if (t >= 0) targets.add(t);
  }
  return targets;
}

function resolveAction(card, target, guess) {
  switch (card) {
    case 1: return GUARD_OFF + target * 7 + (guess - 2);
    case 2: return PRIEST_OFF + target;
    case 3: return BARON_OFF + target;
    case 4: return HANDMAID_ACT;
    case 5: return PRINCE_OFF + target;
    case 6: return KING_OFF + target;
    case 7: return COUNTESS_ACT;
    case 8: return PRINCESS_ACT;
    default: return -1;
  }
}

function submitNoTargetCard(card, legalSet) {
  const aid = resolveAction(card, -1, -1);
  if (legalSet.has(aid)) {
    resetPending();
    currentCtx.submitAction(aid);
  }
}

function resetPending() {
  pendingCard = 0;
  pendingTarget = -1;
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
  const currentPlayer = st.current_player;

  const board = document.createElement('div');
  board.className = 'll-board';

  const deckInfo = document.createElement('div');
  deckInfo.className = 'll-deck-info';
  deckInfo.setAttribute('data-deck', '');
  deckInfo.textContent = '牌堆剩余: ' + st.deck_size;
  if (st.face_up_removed && st.face_up_removed.length > 0) {
    const removed = st.face_up_removed.map(c => CARD_LABELS[c] + '(' + c + ')').join(', ');
    deckInfo.textContent += ' | 公开移除: ' + removed;
  }
  board.appendChild(deckInfo);

  const opponents = document.createElement('div');
  opponents.className = 'll-opponents';
  for (let pi = 0; pi < numPlayers; pi++) {
    if (pi === humanPlayer) continue;
    const p = st.players[pi];
    const opp = document.createElement('div');
    opp.className = 'll-opponent';
    opp.setAttribute('data-opponent', String(pi));
    if (!p.alive) opp.classList.add('eliminated');
    if (p.protected) opp.classList.add('protected');
    if (pi === currentPlayer) opp.classList.add('current-turn');

    const canTarget = playing && pendingCard > 0 && needsTarget(pendingCard) && !needsGuess(pendingCard);
    const validTargets = pendingCard > 0 ? getTargetsForCard(pendingCard, legalSet) : new Set();
    const isValidTarget = canTarget && validTargets.has(pi);

    if (isValidTarget) {
      opp.classList.add('selectable');
      opp.addEventListener('click', () => {
        const aid = resolveAction(pendingCard, pi, -1);
        if (legalSet.has(aid)) {
          resetPending();
          ctx.submitAction(aid);
        }
      });
    }

    if (pendingCard > 0 && pendingTarget === pi) {
      opp.classList.add('selected-target');
    }

    const nameEl = document.createElement('div');
    nameEl.className = 'll-opp-name';
    nameEl.textContent = '玩家' + pi;
    if (!p.alive) nameEl.textContent += ' (淘汰)';
    else if (p.protected) nameEl.textContent += ' (保护中)';
    opp.appendChild(nameEl);

    if (p.discards && p.discards.length > 0) {
      const disc = document.createElement('div');
      disc.className = 'll-opp-discards';
      for (const c of p.discards) {
        const chip = document.createElement('span');
        chip.className = 'll-discard-chip';
        chip.textContent = CARD_LABELS[c] + '(' + c + ')';
        disc.appendChild(chip);
      }
      opp.appendChild(disc);
    }

    opponents.appendChild(opp);
  }
  board.appendChild(opponents);

  if (playing && pendingCard === 1 && pendingTarget === -1) {
    const guardPanel = document.createElement('div');
    guardPanel.className = 'll-guard-panel';
    const validTgts = getTargetsForCard(1, legalSet);
    for (const t of validTgts) {
      const p = st.players[t];
      if (t === humanPlayer) continue;
      const row = document.createElement('div');
      row.className = 'll-guard-target-row';
      const label = document.createElement('span');
      label.textContent = '玩家' + t + ': ';
      row.appendChild(label);
      for (let g = 2; g <= 8; g++) {
        const aid = GUARD_OFF + t * 7 + (g - 2);
        if (!legalSet.has(aid)) continue;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'll-guess-btn';
        btn.textContent = CARD_LABELS[g] + '(' + g + ')';
        btn.addEventListener('click', () => {
          resetPending();
          ctx.submitAction(aid);
        });
        row.appendChild(btn);
      }
      guardPanel.appendChild(row);
    }
    board.appendChild(guardPanel);
  }

  container.appendChild(board);
}

function renderPlayerArea(container, gs, ctx) {
  container.innerHTML = '';
  if (!gs || !gs.state) return;

  const st = gs.state;
  const legalSet = getLegalSet(gs);
  const playing = ctx.canPlay;
  const humanPlayer = ctx.state.humanPlayer;
  const currentPlayer = st.current_player;
  const pd = st.players[humanPlayer];

  const area = document.createElement('div');
  area.className = 'll-player-area';

  if (pd.discards && pd.discards.length > 0) {
    const disc = document.createElement('div');
    disc.className = 'll-my-discards';
    const label = document.createElement('span');
    label.className = 'll-discard-label';
    label.textContent = '已出: ';
    disc.appendChild(label);
    for (const c of pd.discards) {
      const chip = document.createElement('span');
      chip.className = 'll-discard-chip mine';
      chip.textContent = CARD_LABELS[c] + '(' + c + ')';
      disc.appendChild(chip);
    }
    area.appendChild(disc);
  }

  if (!pd.alive) {
    const dead = document.createElement('div');
    dead.className = 'll-dead-msg';
    dead.textContent = '你已被淘汰';
    area.appendChild(dead);
    container.appendChild(area);
    return;
  }

  const hand = document.createElement('div');
  hand.className = 'll-hand';

  const handCard = pd.hand;
  const drawnCard = (currentPlayer === humanPlayer) ? st.drawn_card : 0;
  const cards = drawnCard > 0 ? [handCard, drawnCard] : [handCard];

  for (const c of cards) {
    if (c <= 0) continue;
    const cardEl = createCardElement(c, playing, legalSet);
    hand.appendChild(cardEl);
  }

  area.appendChild(hand);

  if (pendingCard > 0) {
    const hint = document.createElement('div');
    hint.className = 'll-pending-hint';
    if (needsGuess(pendingCard)) {
      hint.textContent = '请选择目标和猜测的牌';
    } else if (needsTarget(pendingCard)) {
      hint.textContent = '请点击目标玩家';
    }
    area.appendChild(hint);
  }

  container.appendChild(area);
}

function createCardElement(cardValue, playing, legalSet) {
  const el = document.createElement('button');
  el.type = 'button';
  el.className = 'll-card card-' + cardValue;
  el.setAttribute('data-hand-card', String(cardValue));
  const playable = playing && canPlayCard(cardValue, legalSet);

  const val = document.createElement('div');
  val.className = 'll-card-value';
  val.textContent = CARD_VALUES[cardValue];
  el.appendChild(val);

  const name = document.createElement('div');
  name.className = 'll-card-name';
  name.textContent = CARD_LABELS[cardValue];
  el.appendChild(name);

  const effect = document.createElement('div');
  effect.className = 'll-card-effect';
  effect.textContent = CARD_EFFECTS[cardValue];
  el.appendChild(effect);

  if (pendingCard === cardValue) {
    el.classList.add('selected');
  }

  if (playable) {
    el.classList.add('playable');
    el.addEventListener('click', () => {
      if (pendingCard === cardValue) {
        resetPending();
        currentCtx.rerender();
        return;
      }
      pendingCard = cardValue;
      pendingTarget = -1;

      if (!needsTarget(cardValue)) {
        submitNoTargetCard(cardValue, legalSet);
      } else {
        currentCtx.rerender();
      }
    });
  } else {
    el.disabled = true;
  }

  return el;
}

function formatMove(info, actionId) {
  if (!info || !info.type) {
    if (actionId === null || actionId === undefined) return '开局';
    return '动作 #' + actionId;
  }
  const cardName = info.card_name || '';
  switch (info.type) {
    case 'guard':
      return cardName + ' -> 玩家' + info.target + ' 猜 ' + (info.guess_name || info.guess);
    case 'priest':
      return cardName + ' -> 偷看玩家' + info.target;
    case 'baron':
      return cardName + ' -> 比较玩家' + info.target;
    case 'handmaid':
      return cardName + ' (保护)';
    case 'prince':
      return cardName + ' -> 玩家' + info.target + '弃牌';
    case 'king':
      return cardName + ' -> 交换玩家' + info.target;
    case 'countess':
      return cardName + ' (弃出)';
    case 'princess':
      return cardName + ' (弃出 = 淘汰)';
    default:
      return '动作 #' + actionId;
  }
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const steps = [];
  const actor = prevState.current_player;
  const cardValue = actionInfo.card || 0;
  const target = actionInfo.target;

  if (cardValue > 0) {
    const toSelector = (target != null && target >= 0)
      ? '[data-opponent="' + target + '"]'
      : '[data-deck]';

    steps.push({
      type: 'fly',
      from: '[data-hand-card="' + cardValue + '"]',
      to: toSelector,
      createElement() {
        const el = document.createElement('div');
        el.className = 'anim-flying-card ll-card-fly';
        el.textContent = CARD_VALUES[cardValue];
        return el;
      },
      duration: 400,
      hideFrom: true,
    });

    if (target != null && target >= 0) {
      steps.push({
        type: 'highlight',
        target: '[data-opponent="' + target + '"]',
        className: 'anim-highlight',
        duration: 500,
      });
    }
  }

  return steps.length ? steps : null;
}

createApp({
  gameId: 'loveletter',
  gameTitle: 'Love Letter',
  gameIntro: '推理对手手牌，猜测、比较、保护，最后存活或手牌最大者获胜',
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
