import { apiGet, API_BASE } from './api.js';

export function createReplayController(infoCol, config) {
  const panel = document.createElement('div');
  panel.className = 'analysis-panel';
  panel.hidden = true;
  panel.innerHTML = `
    <div class="analysis-title">录像分析</div>
    <div class="analysis-controls">
      <button class="replay-btn" data-action="first" type="button">回到最初</button>
      <button class="replay-btn" data-action="play" type="button">播放</button>
      <button class="replay-btn" data-action="prev" type="button">上一步</button>
      <button class="replay-btn" data-action="next" type="button">下一步</button>
      <button class="replay-btn" data-action="warn" type="button">跳到失误</button>
      <button class="replay-btn" data-action="blunder" type="button">跳到严重失误</button>
    </div>
    <div class="analysis-step-card" id="analysis-step">--</div>
    <div class="analysis-list" id="analysis-list"></div>
    <button class="replay-exit-btn" data-action="exit">返回对局</button>
  `;
  infoCol.appendChild(panel);

  let frames = null;
  let step = 0;
  let playing = false;
  let timer = null;
  let onRenderFrame = null;
  let onExit = null;
  let resolveActor = null;

  const stepEl = panel.querySelector('#analysis-step');
  const listEl = panel.querySelector('#analysis-list');
  const btns = {};
  panel.querySelectorAll('[data-action]').forEach(b => { btns[b.dataset.action] = b; });

  function stopTimer() {
    if (timer) { clearInterval(timer); timer = null; }
    playing = false;
  }

  function renderPanel() {
    if (!frames || !frames.length) return;
    const frame = frames[step];
    const total = frames.length;

    const actorName = resolveActor ? resolveActor(frame.actor) : frame.actor;
    const actor = frame.actor === 'start' ? '' : actorName;
    let headerLine = '第 ' + (step + 1) + '/' + total + ' 帧';
    if (actor) headerLine += ' · ' + actor;
    if (frame.tail_solved) headerLine += '（残局已求解）';

    const moveText = config.formatOpponentMove
      ? config.formatOpponentMove(frame.action_info, frame.action_id)
      : (frame.action_id !== null && frame.action_id !== undefined ? '动作 ' + frame.action_id : '开局');
    let moveLine = frame.actor === 'start' ? '开局' : moveText;
    if (frame.is_terminal) moveLine += ' · 终局';

    let text = headerLine + '\n' + moveLine;
    const analysis = frame.analysis;
    if (analysis) {
      const wr = (analysis.best_win_rate * 100).toFixed(1) + '%';
      const drop = analysis.drop_score.toFixed(1) + '%';
      text += '\n预估胜率 ' + wr + ' · 掉点 ' + drop;
      if (frame.action_id !== analysis.best_action && analysis.best_action_info) {
        const bestText = config.formatSuggestedMove
          ? config.formatSuggestedMove(analysis.best_action_info, analysis.best_action)
          : '动作 ' + analysis.best_action;
        text += '\n建议：' + bestText;
      }
    }
    stepEl.textContent = text;

    btns.first.disabled = step <= 0;
    btns.prev.disabled = step <= 0;
    btns.next.disabled = step >= total - 1;
    btns.play.textContent = playing ? '暂停' : '播放';

    let hasBlunder = false, hasWarn = false;
    for (const f of frames) {
      if (f.analysis) {
        if (f.analysis.drop_score >= 10) hasBlunder = true;
        if (f.analysis.drop_score >= 5) hasWarn = true;
      }
    }
    btns.blunder.disabled = !hasBlunder;
    btns.warn.disabled = !hasWarn;

    listEl.innerHTML = '';
    for (let i = 0; i < frames.length; i++) {
      const f = frames[i];
      const item = document.createElement('div');
      item.className = 'analysis-item' + (i === step ? ' current' : '');

      const actorText = f.actor === 'start' ? '' : (resolveActor ? resolveActor(f.actor) : f.actor);
      const frameLabel = '帧 ' + (i + 1) + '/' + total;
      let headerTop = actorText ? (frameLabel + ' · ' + actorText) : frameLabel;
      if (f.tail_solved) headerTop += '（残局已求解）';
      let moveDesc = f.actor === 'start' ? '开局' : (
        config.formatOpponentMove
          ? config.formatOpponentMove(f.action_info, f.action_id)
          : '动作 ' + f.action_id
      );

      const a = f.analysis;
      if (a) {
        const aDrop = a.drop_score;
        const sev = aDrop >= 10 ? 'blunder' : (aDrop >= 5 ? 'warn' : '');
        if (sev) item.classList.add(sev);
        const aWr = (a.best_win_rate * 100).toFixed(1) + '%';
        const aDropTxt = aDrop.toFixed(1) + '%';
        const detailLine = '预估胜率 ' + aWr + ' · 掉点 ' + aDropTxt;
        let bestLine = '';
        if (f.action_id !== a.best_action && a.best_action_info) {
          const bt = config.formatSuggestedMove
            ? config.formatSuggestedMove(a.best_action_info, a.best_action)
            : '动作 ' + a.best_action;
          bestLine = '建议：' + bt;
        }
        item.innerHTML = '<div>' + headerTop + '</div><div>' + moveDesc + '</div><div>' + detailLine + '</div>' + (bestLine ? '<div>' + bestLine + '</div>' : '');
      } else {
        item.innerHTML = '<div>' + headerTop + '</div><div>' + moveDesc + '</div>';
      }

      item.addEventListener('click', () => {
        stopTimer();
        step = i;
        renderPanel();
        if (onRenderFrame) onRenderFrame(frames[step], frames);
      });
      listEl.appendChild(item);
    }

    const currentItem = listEl.querySelector('.current');
    if (currentItem) currentItem.scrollIntoView({ block: 'nearest', behavior: 'smooth' });

    if (onRenderFrame) onRenderFrame(frames[step], frames);
  }

  function jumpToSeverity(minDrop) {
    if (!frames) return;
    const start = step + 1;
    const len = frames.length;
    for (let offset = 0; offset < len; offset++) {
      const i = (start + offset) % len;
      if (frames[i].analysis && frames[i].analysis.drop_score >= minDrop) {
        stopTimer();
        step = i;
        renderPanel();
        return;
      }
    }
  }

  btns.first.addEventListener('click', () => { stopTimer(); step = 0; renderPanel(); });
  btns.prev.addEventListener('click', () => { stopTimer(); step = Math.max(0, step - 1); renderPanel(); });
  btns.next.addEventListener('click', () => { stopTimer(); step = Math.min((frames || []).length - 1, step + 1); renderPanel(); });
  btns.play.addEventListener('click', () => {
    if (playing) { stopTimer(); renderPanel(); return; }
    playing = true;
    renderPanel();
    timer = setInterval(() => {
      const max = (frames || []).length - 1;
      if (step >= max) { stopTimer(); renderPanel(); return; }
      step++;
      renderPanel();
    }, 800);
  });
  btns.blunder.addEventListener('click', () => jumpToSeverity(10));
  btns.warn.addEventListener('click', () => jumpToSeverity(5));
  btns.exit.addEventListener('click', () => { if (onExit) onExit(); });

  return {
    async enter(sessionId) {
      const data = await apiGet(API_BASE + '/' + sessionId + '/replay');
      frames = data.frames;
      step = frames.length - 1;
      playing = false;
      panel.hidden = false;
      renderPanel();
      return data;
    },
    enterWithFrames(framesData) {
      frames = framesData;
      step = 0;
      playing = false;
      panel.hidden = false;
      renderPanel();
    },
    exit() {
      stopTimer();
      frames = null;
      playing = false;
      panel.hidden = true;
    },
    isActive() { return !panel.hidden && frames !== null; },
    onRenderFrame(fn) { onRenderFrame = fn; },
    onExit(fn) { onExit = fn; },
    setResolveActor(fn) { resolveActor = fn; },
    getPanel() { return panel; },
  };
}
