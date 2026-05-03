import { apiGet } from './api.js';

export function createSidebar(sidebarEl, config, callbacks) {
  const difficulties = config.difficulties || ['heuristic', 'casual', 'expert'];
  const defaultDiff = config.defaultDifficulty || 'expert';
  const diffLabels = { heuristic: '启发式', casual: '体验', expert: '专家' };
  const players = config.players || { min: 2, max: 2 };
  const showPlayerCount = players.max > 2;

  let playerCountHtml = '';
  if (showPlayerCount) {
    const btns = [];
    for (let n = players.min; n <= players.max; n++) {
      const active = n === players.min ? ' active' : '';
      btns.push(`<button class="side-btn${active}" data-players="${n}" type="button">${n}人</button>`);
    }
    playerCountHtml = `
      <label>人数</label>
      <div class="player-count-grid">${btns.join('')}</div>`;
  }

  sidebarEl.innerHTML = `
    <h1>DinoBoard</h1>
    <div class="game-switcher-wrap">
      <label for="game-selector">切换游戏</label>
      <select id="game-selector"></select>
    </div>
    <div class="card">
      <h2>开局</h2>
      ${playerCountHtml}
      <label>座次</label>
      <div id="seat-section"></div>
      <label>难度</label>
      <div class="difficulty-grid">
        ${difficulties.map(d =>
          `<button class="side-btn${d === defaultDiff ? ' active' : ''}" data-diff="${d}" type="button">${diffLabels[d] || d}</button>`
        ).join('')}
      </div>
      <button id="btn-start">开始对局</button>
      <div id="start-msg" class="muted"></div>
    </div>
    <div class="card">
      <h2>高级功能</h2>
      <button id="btn-undo">悔棋</button>
      <div id="force-section">
        <button id="btn-force">替对手落子</button>
      </div>
      <button id="btn-hint">智能提示</button>
      <button id="btn-load-replay">加载录像</button>
      <input type="file" id="replay-file-input" accept=".json" style="display:none">
      <div id="ops-msg" class="muted"></div>
    </div>
  `;

  let sideMode = '0';
  let difficulty = defaultDiff;
  let numPlayers = players.min;

  function rebuildSeatButtons() {
    const section = sidebarEl.querySelector('#seat-section');
    const btns = [];
    for (let i = 0; i < numPlayers; i++) {
      const active = i === 0 ? ' active' : '';
      btns.push(`<button class="side-btn${active}" data-seat="${i}" type="button">玩家${i}</button>`);
    }
    btns.push('<button class="side-btn" data-seat="random" type="button">随机</button>');
    section.innerHTML = `<div class="side-grid">${btns.join('')}</div>`;
    sideMode = '0';

    const seatBtns = section.querySelectorAll('[data-seat]');
    seatBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        seatBtns.forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        sideMode = btn.dataset.seat;
      });
    });
  }

  if (showPlayerCount) {
    const playerBtns = sidebarEl.querySelectorAll('[data-players]');
    playerBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        playerBtns.forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        numPlayers = parseInt(btn.dataset.players);
        rebuildSeatButtons();
      });
    });
  }

  rebuildSeatButtons();

  const diffBtns = sidebarEl.querySelectorAll('[data-diff]');
  diffBtns.forEach(btn => {
    btn.addEventListener('click', () => {
      diffBtns.forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      difficulty = btn.dataset.diff;
    });
  });

  sidebarEl.querySelector('#btn-start').addEventListener('click', () => {
    callbacks.onStart(sideMode, difficulty, numPlayers);
  });
  sidebarEl.querySelector('#btn-undo').addEventListener('click', () => callbacks.onUndo());
  sidebarEl.querySelector('#btn-force').addEventListener('click', () => callbacks.onForce());
  sidebarEl.querySelector('#btn-hint').addEventListener('click', () => callbacks.onHint());

  function rebuildForceButtons(aiPlayers) {
    const section = sidebarEl.querySelector('#force-section');
    if (!aiPlayers || aiPlayers.length <= 1) {
      section.innerHTML = '<button id="btn-force">替对手落子</button>';
    } else {
      const btns = aiPlayers.map(p => {
        const label = config.getPlayerSymbol ? config.getPlayerSymbol(p) : '玩家' + p;
        return `<button class="btn-force-player" data-force-player="${p}">替${label}落子</button>`;
      });
      section.innerHTML = btns.join('');
    }
    section.querySelectorAll('#btn-force').forEach(btn => {
      btn.addEventListener('click', () => callbacks.onForce());
    });
    section.querySelectorAll('.btn-force-player').forEach(btn => {
      btn.addEventListener('click', () => callbacks.onForce(parseInt(btn.dataset.forcePlayer)));
    });
  }

  const fileInput = sidebarEl.querySelector('#replay-file-input');
  sidebarEl.querySelector('#btn-load-replay').addEventListener('click', () => {
    fileInput.click();
  });
  fileInput.addEventListener('change', () => {
    const file = fileInput.files[0];
    if (!file) return;
    fileInput.value = '';
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const data = JSON.parse(reader.result);
        if ((!data.frames || !data.frames.length) && !data.action_history) {
          callbacks.onLoadReplay(null, '录像文件中没有 frames 或 action_history');
          return;
        }
        callbacks.onLoadReplay(data);
      } catch (e) {
        callbacks.onLoadReplay(null, '无法解析 JSON: ' + e.message);
      }
    };
    reader.readAsText(file);
  });

  loadGameSwitcher(sidebarEl.querySelector('#game-selector'), config.gameId);

  return {
    setStartMsg(text) { sidebarEl.querySelector('#start-msg').textContent = text; },
    setOpsMsg(text) { sidebarEl.querySelector('#ops-msg').textContent = text; },
    getSideMode() { return sideMode; },
    getDifficulty() { return difficulty; },
    getNumPlayers() { return numPlayers; },
    rebuildForceButtons,
  };
}

async function loadGameSwitcher(sel, currentGameId) {
  try {
    const data = await apiGet('/api/games/available');
    for (const g of data.games) {
      if (!g.has_web) continue;
      const opt = document.createElement('option');
      opt.value = g.game_id;
      opt.textContent = g.display_name;
      if (g.game_id === currentGameId) opt.selected = true;
      sel.appendChild(opt);
    }
    sel.addEventListener('change', () => {
      window.location.href = '/games/' + sel.value + '/';
    });
  } catch (e) { /* ignore */ }
}
