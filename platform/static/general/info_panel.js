export function createInfoPanel(infoCol, config) {
  const panel = document.createElement('div');
  panel.className = 'info-panel';
  panel.innerHTML = `
    <div class="info-pill" id="info-turn">当前轮到：--</div>
    <div class="info-pill" id="info-message">${config.gameIntro || ''}</div>
    <div class="info-pill" id="info-winrate">对手预估胜率：--</div>
    <div class="info-pill" id="info-suggest">AI 提示：--</div>
  `;
  infoCol.appendChild(panel);

  const extensionContainer = document.createElement('div');
  extensionContainer.className = 'info-extensions';
  infoCol.appendChild(extensionContainer);

  const els = {
    turn: panel.querySelector('#info-turn'),
    message: panel.querySelector('#info-message'),
    winrate: panel.querySelector('#info-winrate'),
    suggest: panel.querySelector('#info-suggest'),
  };

  function formatWinrate(wr) {
    if (wr === null || wr === undefined) return '--';
    return (Math.max(0, Math.min(1, wr)) * 100).toFixed(1) + '%';
  }

  return {
    setTurn(text) { els.turn.textContent = text; },
    setMessage(text) { els.message.textContent = text; },
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
      els.message.textContent = config.gameIntro || '';
      els.winrate.textContent = '对手预估胜率：--';
      els.suggest.textContent = 'AI 提示：--';
    },
  };
}
