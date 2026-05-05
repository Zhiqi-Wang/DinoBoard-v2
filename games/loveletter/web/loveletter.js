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
    opp.appendChild(nameEl);

    const statusEl = document.createElement('div');
    statusEl.className = 'll-opp-status';
    if (!p.alive) {
      statusEl.classList.add('status-eliminated');
      statusEl.textContent = '淘汰';
    } else if (p.protected) {
      statusEl.classList.add('status-protected');
      statusEl.textContent = '保护中';
    } else if (pi === currentPlayer) {
      statusEl.classList.add('status-turn');
      statusEl.textContent = '行动中';
    } else {
      statusEl.textContent = ' ';
    }
    opp.appendChild(statusEl);

    const disc = buildDiscardPile(p.discards || [], pi);
    opp.appendChild(disc);

    opponents.appendChild(opp);
  }
  board.appendChild(opponents);

  if (playing && pendingCard === 1 && pendingTarget === -1) {
    const guardPanel = document.createElement('div');
    guardPanel.className = 'll-guard-panel';
    const title = document.createElement('div');
    title.className = 'll-guard-panel-title';
    title.textContent = '侍卫：选择目标并猜测其手牌';
    guardPanel.appendChild(title);
    const validTgts = getTargetsForCard(1, legalSet);
    // Engine appends a self-fallback action (target=me, guess=2) when
    // every opponent is protected. Skip self here — it becomes a
    // one-button "discard with no effect" row instead.
    let rowsAdded = 0;
    for (const t of validTgts) {
      if (t === humanPlayer) continue;
      const row = document.createElement('div');
      row.className = 'll-guard-target-row';
      const label = document.createElement('span');
      label.className = 'll-guard-target-label';
      label.textContent = '玩家' + t;
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
      rowsAdded++;
    }
    if (rowsAdded === 0) {
      const row = document.createElement('div');
      row.className = 'll-guard-target-row';
      const label = document.createElement('span');
      label.className = 'll-guard-target-label';
      label.textContent = '所有对手被保护';
      row.appendChild(label);
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'll-guess-btn';
      btn.textContent = '弃出无效';
      btn.addEventListener('click', () => {
        const aid = GUARD_OFF + humanPlayer * 7;
        if (legalSet.has(aid)) {
          resetPending();
          ctx.submitAction(aid);
        }
      });
      row.appendChild(btn);
      guardPanel.appendChild(row);
    }
    board.appendChild(guardPanel);
  }

  container.appendChild(board);
}

function buildDiscardPile(discards, playerIdx) {
  // Card-chip style pile: each discarded card shows up as a small card
  // rather than a text chip, so plays visually land as physical cards.
  // An empty trailing anchor (.incoming-slot) gives describeTransition
  // a stable target for fly-to-discard animations before the data
  // re-render adds the new card chip.
  const wrap = document.createElement('div');
  wrap.className = 'll-discard-pile';
  wrap.setAttribute('data-discard-pile', String(playerIdx));
  for (let i = 0; i < discards.length; i++) {
    const c = discards[i];
    const card = document.createElement('div');
    card.className = 'll-discard-card card-' + c;
    card.setAttribute('data-discard-slot', playerIdx + '-' + i);
    const v = document.createElement('div');
    v.className = 'll-discard-card-value';
    v.textContent = CARD_VALUES[c];
    card.appendChild(v);
    const n = document.createElement('div');
    n.className = 'll-discard-card-name';
    n.textContent = CARD_LABELS[c];
    card.appendChild(n);
    wrap.appendChild(card);
  }
  const incoming = document.createElement('div');
  incoming.className = 'll-discard-card incoming-slot';
  incoming.setAttribute('data-discard-incoming', String(playerIdx));
  wrap.appendChild(incoming);
  return wrap;
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
  area.setAttribute('data-player', String(humanPlayer));

  // Self-target highlight: Prince can always target self; other target
  // cards (Priest/Baron/King) can only "target self" via the engine's
  // no-effect fallback when every opponent is Handmaid-protected.
  // `validTargets` already encodes both cases (engine adds offset+me
  // when `found==false`).
  const selfIsValidTarget = (
    playing && pendingCard > 0 && needsTarget(pendingCard) && !needsGuess(pendingCard)
    && getTargetsForCard(pendingCard, legalSet).has(humanPlayer)
  );
  if (selfIsValidTarget) {
    area.classList.add('selectable-self');
    area.addEventListener('click', () => {
      const aid = resolveAction(pendingCard, humanPlayer, -1);
      if (legalSet.has(aid)) {
        resetPending();
        ctx.submitAction(aid);
      }
    });
  }

  const disc = buildDiscardPile(pd.discards || [], humanPlayer);
  disc.classList.add('mine');
  area.appendChild(disc);

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
      hint.textContent = selfIsValidTarget && pendingCard === 5
        ? '请点击目标玩家（可点击你自己）'
        : selfIsValidTarget
          ? '所有对手被保护，点击自己弃出无效'
          : '请点击目标玩家';
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

function actorSelector(actor, humanPlayer) {
  return actor === humanPlayer
    ? '[data-player="' + actor + '"]'
    : '[data-opponent="' + actor + '"]';
}

function describeTransition(prevState, newState, actionInfo, actionId) {
  if (!actionInfo || !prevState || !prevState.state) return null;

  const steps = [];
  const actor = prevState.current_player;
  const humanPlayer = (currentCtx && currentCtx.state) ? currentCtx.state.humanPlayer : 0;
  const cardValue = actionInfo.card || 0;
  const target = actionInfo.target;

  // Step 1: the card physically moves to the actor's discard pile, with
  // the action-bubble popping above them. This resolves BEFORE any target
  // highlight or reveal modal, so the player always sees "card A lands
  // in the play area" before the game adjudicates its effect. Without
  // this split the fly, the highlight, and the reveal all race each
  // other and the source card appears to vanish mid-air.
  const playGroup = { type: 'group', children: [] };
  playGroup.children.push({
    type: 'popup',
    target: actorSelector(actor, humanPlayer),
    content: formatMove(actionInfo, actionId),
    className: 'action-bubble',
    width: 220,
    height: 36,
    duration: 1500,
  });
  // Shared onComplete: when the card lands in the incoming-slot anchor,
  // materialize a real discard-card DOM node INTO that anchor so the
  // discard pile visibly grows by one. Without this the incoming anchor
  // stays empty between the fly finishing and the final re-render, so
  // any subsequent step (target highlight, baron reveal modal) shows a
  // discard pile missing the card that just flew in — §3 "中间状态维护"
  // violation. We mutate DOM only; framework will re-render to real
  // state after all steps finish, which replaces our temporary node.
  const materializeInDiscard = (actor, cardValue) => () => {
    const incoming = document.querySelector('[data-discard-incoming="' + actor + '"]');
    if (!incoming) return;
    const card = document.createElement('div');
    card.className = 'll-discard-card card-' + cardValue;
    const v = document.createElement('div');
    v.className = 'll-discard-card-value';
    v.textContent = CARD_VALUES[cardValue];
    const n = document.createElement('div');
    n.className = 'll-discard-card-name';
    n.textContent = CARD_LABELS[cardValue];
    card.appendChild(v);
    card.appendChild(n);
    // Insert before the incoming anchor so the pile grows leftward of it,
    // matching buildDiscardPile's output ordering.
    incoming.parentNode.insertBefore(card, incoming);
  };

  if (cardValue > 0 && actor === humanPlayer) {
    playGroup.children.push({
      type: 'fly',
      from: '[data-hand-card="' + cardValue + '"]',
      to: '[data-discard-incoming="' + actor + '"]',
      createElement() {
        const el = document.createElement('div');
        el.className = 'anim-flying-card ll-card-fly card-' + cardValue;
        el.textContent = CARD_VALUES[cardValue];
        return el;
      },
      duration: 450,
      hideFrom: true,
      onComplete: materializeInDiscard(actor, cardValue),
    });
  } else if (cardValue > 0 && actor !== humanPlayer) {
    playGroup.children.push({
      type: 'fly',
      from: '[data-opponent="' + actor + '"]',
      to: '[data-discard-incoming="' + actor + '"]',
      createElement() {
        const el = document.createElement('div');
        el.className = 'anim-flying-card ll-card-fly card-' + cardValue;
        el.textContent = CARD_VALUES[cardValue];
        return el;
      },
      width: 68,
      height: 92,
      duration: 450,
      onComplete: materializeInDiscard(actor, cardValue),
    });
  }
  if (playGroup.children.length) steps.push(playGroup);

  // Step 2: target highlight runs after the play animation finishes, so
  // the player reads "card X is now in the pile, and it's targeting
  // player Y" as a sequence rather than a blur.
  if (target != null && target >= 0 && target !== actor) {
    const highlightSel = target === humanPlayer
      ? '[data-player="' + target + '"]'
      : '[data-opponent="' + target + '"]';
    steps.push({
      type: 'highlight',
      target: highlightSel,
      className: 'anim-highlight',
      duration: 600,
    });
  }

  // Information reveals: Priest peek and Baron compare expose private
  // cards to the perspective player. A head-bubble popup isn't enough
  // — the player must actually absorb the info before the next AI turn
  // starts, or the revealed card flashes by and is lost. Use a blocking
  // reveal-modal step that requires acknowledgement. See §3.7 of the
  // design doc for when to use this.
  const reveal = buildRevealStep(prevState, newState, actionInfo, actor, humanPlayer);
  if (reveal) steps.push(reveal);

  return steps.length ? steps : null;
}

function buildRevealStep(prevState, newState, actionInfo, actor, humanPlayer) {
  const type = actionInfo && actionInfo.type;
  if (type !== 'priest' && type !== 'baron') return null;

  const target = actionInfo.target;
  if (target == null || target < 0) return null;

  // Only interrupt the player when they're actually learning something.
  // - Priest: only the actor learns. Skip unless human is the actor.
  // - Baron: both compared hands become known to actor and target.
  //   Skip if neither is the human.
  const humanIsActor = actor === humanPlayer;
  const humanIsTarget = target === humanPlayer;
  if (type === 'priest' && !humanIsActor) return null;
  if (type === 'baron' && !humanIsActor && !humanIsTarget) return null;

  const prevPlayers = prevState.state.players || [];
  const newPlayers = (newState && newState.state && newState.state.players) || [];

  const targetPrevHand = prevPlayers[target] ? prevPlayers[target].hand : 0;

  if (type === 'priest') {
    if (targetPrevHand <= 0) return null;
    return {
      type: 'reveal',
      title: '牧师 · 偷看手牌',
      body: revealBody([
        { label: '玩家' + target + '的手牌', card: targetPrevHand },
      ], '只有你看到了这张牌。'),
    };
  }

  // Baron: compare actor's kept card (after playing Baron, they retain
  // their OTHER card) with target's hand. We read what each player
  // actually held at compare time:
  //   - The actor played Baron. Their compare card is whichever of
  //     {hand, drawn_card} wasn't the Baron (value 3). In prevState
  //     the current_player's drawn_card is visible; their hand is the
  //     permanent one.
  //   - The target's hand is simply prevPlayers[target].hand.
  const actorKept = baronActorKeptCard(prevState, actor);
  if (actorKept <= 0 || targetPrevHand <= 0) return null;

  const actorWon = actorKept > targetPrevHand;
  const tie = actorKept === targetPrevHand;
  const humanSide = humanIsActor ? 'actor' : 'target';
  // "outcomeText" from the human's perspective: won / lost / tied.
  const outcomeText = tie
    ? '平局，双方都不淘汰'
    : (humanSide === 'actor'
        ? (actorWon ? '你的牌更大，玩家' + target + ' 被淘汰' : '玩家' + target + ' 的牌更大，你被淘汰')
        : (actorWon ? '玩家' + actor + ' 的牌更大，你被淘汰' : '你的牌更大，玩家' + actor + ' 被淘汰'));

  return {
    type: 'reveal',
    title: '男爵 · 比较手牌',
    body: revealBody([
      { label: '玩家' + actor + ' 的手牌', card: actorKept },
      { label: '玩家' + target + ' 的手牌', card: targetPrevHand },
    ], outcomeText),
  };
}

function baronActorKeptCard(prevState, actor) {
  const st = prevState.state;
  const p = st.players && st.players[actor];
  if (!p) return 0;
  const hand = p.hand || 0;
  // drawn_card in prevState.state is only populated for current_player.
  // In the pre-action snapshot, current_player == actor, so this is the
  // two-card hand the actor just picked from. Whichever isn't Baron (3)
  // is the card they kept to compare.
  const drawn = st.drawn_card || 0;
  if (hand === 3 && drawn > 0) return drawn;
  if (drawn === 3 && hand > 0) return hand;
  // Edge: neither equals Baron — shouldn't happen legally, but fall
  // back to whichever is the higher value so the reveal still shows
  // something sensible.
  return Math.max(hand, drawn);
}

function revealBody(cards, summaryText) {
  const wrap = document.createElement('div');
  wrap.style.display = 'flex';
  wrap.style.flexDirection = 'column';
  wrap.style.alignItems = 'center';
  wrap.style.gap = '12px';

  const row = document.createElement('div');
  row.style.display = 'flex';
  row.style.gap = '14px';
  row.style.justifyContent = 'center';
  for (const c of cards) {
    const item = document.createElement('div');
    item.style.display = 'flex';
    item.style.flexDirection = 'column';
    item.style.alignItems = 'center';
    item.style.gap = '6px';
    const label = document.createElement('div');
    label.style.fontSize = '12px';
    label.style.color = '#475569';
    label.textContent = c.label;
    item.appendChild(label);
    const card = document.createElement('div');
    card.className = 'll-reveal-card card-' + c.card;
    const val = document.createElement('div');
    val.className = 'll-reveal-card-value';
    val.textContent = CARD_VALUES[c.card];
    const name = document.createElement('div');
    name.className = 'll-reveal-card-name';
    name.textContent = CARD_LABELS[c.card];
    card.appendChild(val);
    card.appendChild(name);
    item.appendChild(card);
    row.appendChild(item);
  }
  wrap.appendChild(row);

  if (summaryText) {
    const summary = document.createElement('div');
    summary.style.fontSize = '13px';
    summary.style.color = '#334155';
    summary.textContent = summaryText;
    wrap.appendChild(summary);
  }
  return wrap;
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
