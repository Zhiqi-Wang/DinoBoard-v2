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

  // Stage / deck info used to live in an in-board info bar whose width
  // shifted between stages — moved to the sidebar-info extension (see
  // `extensions` in createApp) so the game area stays visually stable.
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

  // Always render the action-panel slot — even when it's the AI's turn or
  // the human has nothing to decide — so the section below the players
  // area keeps a stable position and height. Each stage swaps in its own
  // button row; illegal actions are disabled rather than removed.
  let panel = null;
  if (playing && stage === 0) {
    panel = renderDeclareActions(st, legalSet, humanPlayer, ctx);
  } else if (playing && (stage === 1 || stage === 5)) {
    panel = renderChallengePanel(legalSet, ctx);
  } else if (playing && stage === 4) {
    panel = renderCounterPanel(st, legalSet, ctx);
  } else if (playing && (stage === 2 || stage === 6)) {
    panel = renderRevealPanel(st, legalSet, humanPlayer, ctx, '亮牌证明');
  } else if (playing && (stage === 3 || stage === 7 || stage === 10)) {
    panel = renderRevealPanel(st, legalSet, humanPlayer, ctx, '选择失去影响力');
  } else if (playing && (stage === 8 || stage === 9)) {
    panel = renderExchangePanel(st, legalSet, ctx);
  } else {
    panel = renderIdlePanel(st, humanPlayer);
  }
  board.appendChild(panel);

  container.appendChild(board);
}

function renderIdlePanel(st, humanPlayer) {
  // Placeholder panel shown when it's not the human's turn or when the
  // current stage doesn't require human input. Keeps the action-bar slot
  // occupying the same footprint as any real panel.
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel idle';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  const actor = (st.active_player != null) ? st.active_player : st.current_player;
  title.textContent = actor === humanPlayer
    ? '等待对手行动...'
    : '等待玩家' + actor + '行动...';
  panel.appendChild(title);

  // Empty button row matching the real panels' layout — gives the panel
  // the same vertical footprint so switching into a real stage doesn't
  // jolt the layout.
  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';
  const spacer = document.createElement('div');
  spacer.className = 'coup-action-btn-spacer';
  spacer.innerHTML = '&nbsp;';
  btns.appendChild(spacer);
  panel.appendChild(btns);

  return panel;
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

  // Render ALL 7 declare-stage buttons in a fixed order. Illegal actions
  // (coins<7 → coup disabled; coins≥10 → everything except coup disabled)
  // are still rendered in their slot, only greyed out. This keeps the
  // action bar shape stable across turns so the layout doesn't jump.
  const actions = [
    { id: INCOME, label: '收入', desc: '+1💰', type: 'safe' },
    { id: FOREIGN_AID, label: '外援', desc: '+2💰', type: 'safe' },
    { id: TAX, label: '征税', desc: '+3💰 (公爵)', type: 'bluff' },
    { id: EXCHANGE, label: '交换', desc: '换牌 (大使)', type: 'bluff' },
  ];

  for (const a of actions) {
    const legal = legalSet.has(a.id);
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn ' + a.type;
    btn.disabled = !legal;
    btn.innerHTML = '<span class="coup-btn-label">' + a.label + '</span>'
      + '<span class="coup-btn-desc">' + a.desc + '</span>';
    if (legal) {
      btn.addEventListener('click', () => {
        resetPending();
        ctx.submitAction(a.id);
      });
    }
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
    const btn = document.createElement('button');
    btn.className = 'coup-action-btn ' + ta.cls;
    btn.disabled = !hasAny;
    if (pendingActionType === ta.type) btn.classList.add('selected');
    btn.innerHTML = '<span class="coup-btn-label">' + ta.label + '</span>'
      + '<span class="coup-btn-desc">' + ta.desc + '</span>';
    if (hasAny) {
      btn.addEventListener('click', () => {
        if (pendingActionType === ta.type) {
          resetPending();
          ctx.rerender();
        } else {
          pendingActionType = ta.type;
          ctx.rerender();
        }
      });
    }
    btns.appendChild(btn);
  }

  panel.appendChild(btns);

  // Reserved hint slot so the panel height doesn't pop when pendingActionType
  // appears / disappears.
  const hint = document.createElement('div');
  hint.className = 'coup-pending-hint';
  hint.textContent = pendingActionType ? '请点击目标玩家' : ' ';
  if (!pendingActionType) hint.style.visibility = 'hidden';
  panel.appendChild(hint);

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
  // The return-card input lives in the player-area hand (every drawn/held
  // card is clickable) — see renderPlayerArea. This panel only shows the
  // prompt and keeps the action-bar footprint stable.
  const panel = document.createElement('div');
  panel.className = 'coup-action-panel';

  const title = document.createElement('div');
  title.className = 'coup-panel-title';
  title.textContent = st.stage === 8 ? '点击手牌还回第1张' : '点击手牌还回第2张';
  panel.appendChild(title);

  const btns = document.createElement('div');
  btns.className = 'coup-action-btns';
  const spacer = document.createElement('div');
  spacer.className = 'coup-action-btn-spacer';
  spacer.innerHTML = '&nbsp;';
  btns.appendChild(spacer);
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
  area.setAttribute('data-player', String(humanPlayer));

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

  // Exchange stages (8, 9): the active player is holding up to 2 extra
  // drawn cards alongside their 2 hand cards. Show them all in the hand
  // row so the player physically sees "4 cards, pick one to return"
  // matching the tabletop feel, instead of a separate return-button row.
  const drawn = st.exchange_drawn || [];
  const inExchange = (st.stage === 8 || st.stage === 9)
    && st.active_player === humanPlayer
    && ctx.canPlay;
  const legalSet = getLegalSet(gs);

  // Hand cards first (in-hand slots 0, 1). During exchange stage 9, a hand
  // slot may hold character=-1 if the first returned card came out of that
  // slot — skip those so we don't render a blank tile.
  for (let sl = 0; sl < 2; sl++) {
    const inf = p.influences[sl];
    if (!inf.revealed && inf.character < 0) continue;
    const card = buildMyCard(inf.character, inf.revealed, sl, 'hand');
    if (inExchange && !inf.revealed) {
      wireReturnClick(card, inf.character, legalSet, ctx);
    }
    hand.appendChild(card);
  }

  // Drawn cards after, tagged as draw-0 / draw-1 so animation selectors
  // can target them individually.
  for (let di = 0; di < 2; di++) {
    const c = drawn[di];
    if (c == null || c < 0) continue;
    const card = buildMyCard(c, false, di, 'draw');
    card.classList.add('drawn');
    if (inExchange) {
      wireReturnClick(card, c, legalSet, ctx);
    }
    hand.appendChild(card);
  }

  area.appendChild(hand);

  if (inExchange) {
    const hint = document.createElement('div');
    hint.className = 'coup-exchange-hint';
    hint.textContent = st.stage === 8 ? '选择还回第1张牌' : '选择还回第2张牌';
    area.appendChild(hint);
  }

  container.appendChild(area);
}

// Build one card DOM node for the human's hand/draw row. `kind` is
// 'hand' (slot index 0/1 of the real influences) or 'draw' (slot index
// 0/1 of exchange_drawn). Sets data-mycard=<kind>-<slot> so animations
// (return_card fly) can find the right node.
function buildMyCard(character, revealed, slot, kind) {
  const card = document.createElement('div');
  card.className = 'coup-my-card';
  card.setAttribute('data-mycard', kind + '-' + slot);
  if (revealed) {
    card.classList.add('revealed');
    card.textContent = CHAR_LABELS[character];
    card.style.borderColor = '#555';
    return card;
  }
  card.style.borderColor = CHAR_COLORS[character];
  card.style.background = 'linear-gradient(135deg, ' + CHAR_COLORS[character] + '22, #1e1e3a)';
  const name = document.createElement('div');
  name.className = 'coup-mycard-name';
  name.textContent = CHAR_LABELS[character];
  card.appendChild(name);
  const enName = document.createElement('div');
  enName.className = 'coup-mycard-en';
  enName.textContent = CHAR_NAMES[character];
  card.appendChild(enName);
  return card;
}

function wireReturnClick(card, character, legalSet, ctx) {
  const returnId = RETURN_DUKE + character;
  if (!legalSet.has(returnId)) return;
  card.classList.add('returnable');
  card.addEventListener('click', () => ctx.submitAction(returnId));
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

function actorSelector(actor) {
  // Opponent cards (renderBoard) and the human's area (renderPlayerArea)
  // both carry data-player="<idx>", so one selector works for any actor.
  return '[data-player="' + actor + '"]';
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const type = actionInfo.type;
  const actor = prevState.current_player;
  const target = actionInfo.target;

  // Every action gets a head-bubble popup so info-only actions (challenge,
  // allow, block declarations, reveal, return_card) still read visually
  // — without it AI turns flash past too fast to follow. See §3.6 of
  // docs/WEB_DESIGN_PRINCIPLES.md.
  const group = { type: 'group', children: [] };
  group.children.push({
    type: 'popup',
    target: actorSelector(actor),
    content: formatMove(actionInfo, actionId),
    className: 'action-bubble',
    width: 220,
    height: 36,
    duration: 1500,
  });

  if (type === 'coup' || type === 'assassinate') {
    if (target != null && target >= 0) {
      group.children.push({
        type: 'highlight',
        target: '[data-player="' + target + '"]',
        className: 'anim-highlight',
        duration: 600,
      });
    }
  } else if (type === 'steal') {
    if (target != null && target >= 0) {
      group.children.push({
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
      group.children.push({
        type: 'highlight',
        target: '[data-influence="' + actor + '-' + slot + '"]',
        className: 'anim-highlight',
        duration: 600,
      });
    }
  } else if (type === 'return_card') {
    // Ambassador returning a drawn card back to the deck. The card
    // "travels to the middle and disappears". Target is the opponent's
    // hand area for AI actors, or the human's own hand for the human;
    // the card flies from the drawn slot toward the top-of-board deck
    // indicator and fades out. The actual card identity is only visible
    // to the actor (they chose it), so opponent returns just show a
    // generic card back flying.
    const humanPlayer = currentCtx && currentCtx.state ? currentCtx.state.humanPlayer : -1;
    const isHuman = actor === humanPlayer;
    const charId = (actionInfo.character != null && actionInfo.character >= 0) ? actionInfo.character : -1;
    // Prefer the specific slot DOM node the human actually clicked (the
    // one that went from holding `charId` to empty across the transition).
    // A return can come from either a hand slot OR a drawn slot, so diff
    // both against prev state to find the origin. Fall back to the hand
    // row if we can't identify it. Opponents fly from their hand area.
    let fromSel;
    if (isHuman && charId >= 0) {
      const prevSt = prevState.state;
      const newSt = newState.state;
      const prevP = prevSt.players[actor];
      const newP = newSt.players[actor];
      let located = null;
      // Hand slots: was unrevealed + charId, now character==-1 (placeholder)
      // or revealed (shouldn't happen for return but guard anyway).
      for (let sl = 0; sl < 2; sl++) {
        const was = prevP && prevP.influences && prevP.influences[sl];
        const now = newP && newP.influences && newP.influences[sl];
        if (!was || !now) continue;
        if (!was.revealed && was.character === charId && now.character < 0) {
          located = '[data-mycard="hand-' + sl + '"]';
          break;
        }
      }
      // Drawn slots: was drawn[i]==charId, now drawn[i]==-1.
      if (!located) {
        const prevDr = prevSt.exchange_drawn || [];
        const newDr = newSt.exchange_drawn || [];
        for (let di = 0; di < 2; di++) {
          if (prevDr[di] === charId && (newDr[di] == null || newDr[di] < 0)) {
            located = '[data-mycard="draw-' + di + '"]';
            break;
          }
        }
      }
      fromSel = located
        ? (document.querySelector(located) || document.querySelector('.coup-my-hand'))
        : document.querySelector('.coup-my-hand');
    } else {
      fromSel = actorSelector(actor);
    }
    // Destination: the center of the board container — "the card goes
    // back to the middle of the table and disappears".
    const toSel = '.coup-board';
    group.children.push({
      type: 'fly',
      from: fromSel,
      to: toSel,
      width: 84,
      height: 118,
      duration: 500,
      hideFrom: isHuman,
      createElement() {
        const el = document.createElement('div');
        el.className = 'anim-flying-card coup-card-fly';
        if (isHuman && charId >= 0) {
          el.style.borderColor = CHAR_COLORS[charId];
          el.style.background = 'linear-gradient(135deg, ' + CHAR_COLORS[charId] + '55, #1e1e3a)';
          el.innerHTML = '<div style="font-size:13px; font-weight:700;">' + CHAR_LABELS[charId] + '</div>';
        } else {
          el.style.background = '#2a2a4a';
          el.style.borderColor = '#555';
          el.textContent = '?';
        }
        return el;
      },
    });
  }

  const steps = [group];

  // Challenge-reveal: if the human challenged someone and the revealer is
  // not the human, surface what was shown with a blocking modal so the
  // player doesn't miss it between AI turns. See §3.7.
  //
  // Additionally, when the challenge FAILED (revealed role matches the
  // claim — prevSt.revealed stays false across the transition because
  // the rules reshuffle the shown card into the deck and draw a fresh
  // one), animate a reshuffle: the revealed character card flies to the
  // deck and a face-down replacement flies back to the revealer's slot.
  // Fires for both human- and AI-revealer paths so the player always
  // sees the draw.
  if (type === 'reveal') {
    const humanPlayer = currentCtx && currentCtx.state ? currentCtx.state.humanPlayer : -1;
    const challenger = prevState.state.challenger;
    const revealer = actor;
    const slot = actionInfo.slot;
    const prevSt = prevState.state;
    const newSt = newState.state;
    if (slot != null) {
      const pPrev = prevSt.players[revealer];
      const infPrev = pPrev && pPrev.influences && pPrev.influences[slot];
      const revealedChar = infPrev ? infPrev.character : -1;
      const wasRevealed = infPrev ? infPrev.revealed : false;
      const nowRevealed = newSt.players[revealer].influences[slot].revealed;
      const challengeFailed = !wasRevealed && !nowRevealed; // reshuffle case
      // Modal first (blocks on user confirm), THEN the reshuffle animation —
      // otherwise the player would see the card fly away to the deck before
      // the modal even appears, defeating the "make sure they saw it" intent.
      if (challenger === humanPlayer && revealer !== humanPlayer &&
          revealedChar >= 0 && revealedChar < 5) {
        steps.push(buildChallengeRevealStep(revealer, revealedChar, prevSt));
      }
      if (challengeFailed && revealedChar >= 0 && revealedChar < 5) {
        steps.push(buildReshuffleStep(revealer, slot, revealedChar));
      }
    }
  }

  return steps;
}

// Reshuffle animation after a failed challenge: shown card flies to the
// center (deck), then a face-down replacement flies back to the slot.
// Sequential group via two `fly` steps in an outer group with `mode:
// "sequence"`. The revealer's slot DOM uses data-influence="<p>-<slot>".
function buildReshuffleStep(revealer, slot, revealedChar) {
  const slotSel = '[data-influence="' + revealer + '-' + slot + '"]';
  const deckSel = '.coup-board';
  return {
    type: 'group',
    mode: 'sequence',
    children: [
      {
        type: 'fly',
        from: slotSel,
        to: deckSel,
        width: 84,
        height: 118,
        duration: 500,
        hideFrom: true,
        createElement() {
          const el = document.createElement('div');
          el.className = 'anim-flying-card coup-card-fly';
          el.style.borderColor = CHAR_COLORS[revealedChar];
          el.style.background = 'linear-gradient(135deg, ' + CHAR_COLORS[revealedChar] + '55, #1e1e3a)';
          el.innerHTML = '<div style="font-size:13px; font-weight:700;">'
            + CHAR_LABELS[revealedChar] + '</div>';
          return el;
        },
      },
      {
        type: 'fly',
        from: deckSel,
        to: slotSel,
        width: 64,
        height: 44,
        duration: 500,
        hideTo: true,
        createElement() {
          const el = document.createElement('div');
          el.className = 'anim-flying-card coup-card-fly';
          el.style.background = '#2a2a4a';
          el.style.borderColor = '#555';
          el.style.color = '#888';
          el.textContent = '?';
          return el;
        },
      },
    ],
  };
}

function buildChallengeRevealStep(revealer, revealedChar, prevSt) {
  // Decide success/failure by comparing the revealed role to whatever
  // the revealer had just claimed. In kResolveChallengeAction (stage 2)
  // the claim is declared_action's role; in kResolveChallengeCounter
  // (stage 6) the claim is the declared block's role. If they match,
  // the challenge FAILED and the challenger will lose influence.
  const claimedRole = roleClaimedByRevealer(prevSt);
  const matched = claimedRole != null && claimedRole === revealedChar;
  const body = document.createElement('div');
  body.style.cssText = 'display:flex; flex-direction:column; align-items:center; gap:10px;';

  // Render the revealed card with the same visual language as the human's
  // own hand cards (buildMyCard) so the chip is immediately recognizable
  // as a character. Wrap it to add a drop shadow matching the old chip.
  const chipWrap = document.createElement('div');
  chipWrap.style.cssText = 'filter: drop-shadow(0 6px 18px rgba(0,0,0,0.35));';
  const chip = buildMyCard(revealedChar, false, 0, 'reveal');
  chipWrap.appendChild(chip);
  body.appendChild(chipWrap);

  const caption = document.createElement('div');
  caption.style.cssText = 'font-size:14px; color:#e2e8f0; text-align:center;';
  let captionText = '玩家' + revealer + ' 亮出 ' + CHAR_LABELS[revealedChar];
  if (claimedRole != null) {
    captionText += matched
      ? '，质疑失败（你将失去影响力）'
      : '，质疑成功（对手将失去影响力）';
  }
  caption.textContent = captionText;
  body.appendChild(caption);

  return {
    type: 'reveal',
    title: '质疑 · 对方亮牌',
    body,
  };
}

function roleClaimedByRevealer(prevSt) {
  const stage = prevSt.stage;
  // Stage 2 (kResolveChallengeAction): revealer is the actor, claim comes from declared_action.
  if (stage === 2) {
    const a = prevSt.declared_action;
    if (a === 6) return 0; // tax → Duke
    if (a >= 7 && a <= 10) return 1; // assassinate → Assassin
    if (a >= 11 && a <= 14) return 2; // steal → Captain
    if (a === 15) return 3; // exchange → Ambassador
  }
  // Stage 6 (kResolveChallengeCounter): revealer is the blocker, claim is the block role.
  if (stage === 6) {
    const a = prevSt.declared_action;
    // Block-declaration action id isn't stored separately; infer from context:
    // if the base action was foreign_aid (1), block is Duke;
    // if assassinate, block is Contessa;
    // if steal, block is Captain or Ambassador — we can't tell here without
    // storing the block choice, so leave claimed=null in that case.
    if (a === 1) return 0; // foreign_aid block → Duke
    if (a >= 7 && a <= 10) return 4; // assassinate block → Contessa
    // Steal block (Ambassador OR Captain): ambiguous from state alone; skip.
  }
  return null;
}

// Stage + deck info pill shown as the 5th info-panel row. Lives in the
// sidebar rather than the game area so the game-area layout doesn't
// jump when the stage description changes width. See
// WEB_DESIGN_PRINCIPLES §1.5 for the design rule.
const stageExtension = {
  render(el, gameState) {
    const st = gameState && gameState.state ? gameState.state : null;
    if (!st) {
      el.textContent = '阶段：--';
      return;
    }
    const stage = st.stage != null ? st.stage : 0;
    const deck = st.deck_size != null ? st.deck_size : '--';
    el.innerHTML = '<span class="coup-stage">阶段：' + (STAGE_NAMES[stage] || '--') + '</span>'
      + '<span class="coup-deck">牌堆：' + deck + '</span>';
  },
};

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
  extensions: [stageExtension],
  onActionSubmitted: () => { resetPending(); },
  onGameStart: () => { resetPending(); },
  onUndo: () => { resetPending(); },
});
