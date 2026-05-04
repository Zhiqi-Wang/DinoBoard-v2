export function createInfoPanel(infoCol, config) {
  const panel = document.createElement('div');
  panel.className = 'info-panel';
  // 4 pills: turn, opponent-move, AI-winrate, AI-suggest. Each pill has a
  // fixed prefix label so the semantics are clear even when the value
  // part is empty/--. The "对手动作" pill shows only what the opponent
  // last played (human's own moves don't appear here); transient app
  // messages ("已悔棋", "替X落子", etc.) go to sidebar.setOpsMsg.
  panel.innerHTML = `
    <div class="info-pill" id="info-turn">当前轮到：--</div>
    <div class="info-pill" id="info-opp">对手动作：--</div>
    <div class="info-pill" id="info-winrate">对手预估胜率：--</div>
    <div class="info-pill" id="info-suggest">AI 提示：--</div>
  `;
  infoCol.appendChild(panel);

  const extensionContainer = document.createElement('div');
  extensionContainer.className = 'info-extensions';
  infoCol.appendChild(extensionContainer);

  const els = {
    turn: panel.querySelector('#info-turn'),
    opp: panel.querySelector('#info-opp'),
    winrate: panel.querySelector('#info-winrate'),
    suggest: panel.querySelector('#info-suggest'),
  };

  function formatWinrate(wr) {
    if (wr === null || wr === undefined) return '--';
    return (Math.max(0, Math.min(1, wr)) * 100).toFixed(1) + '%';
  }

  return {
    setTurn(text) { els.turn.textContent = text; },
    // No-op kept so game code calling ctx.setInfoStatus() doesn't crash
    // — the dedicated status pill was removed per UI redesign. Games
    // that need a transient prompt should route to sidebar.setOpsMsg.
    setStatus(_text) {},
    // Shows the opponent's latest action. `null`/empty resets to "--".
    setMessage(text) {
      els.opp.textContent = '对手动作：' + (text && text.length ? text : '--');
    },
    setWinrate(wr) { els.winrate.textContent = '对手预估胜率：' + formatWinrate(wr); },
    setSuggest(text) { els.suggest.textContent = 'AI 提示：' + (text || '--'); },
    setVisible(visible) {
      panel.style.display = visible ? '' : 'none';
    },
    updateExtensions(gameState, extensions) {
      extensionContainer.innerHTML = '';
      if (!extensions || !extensions.length) return;
      for (const ext of extensions) {
        const el = document.createElement('div');
        el.className = 'info-pill info-extension';
        ext.render(el, gameState);
        extensionContainer.appendChild(el);
      }
    },
    reset() {
      els.turn.textContent = '当前轮到：--';
      els.opp.textContent = '对手动作：--';
      els.winrate.textContent = '对手预估胜率：--';
      els.suggest.textContent = 'AI 提示：--';
    },
  };
}
