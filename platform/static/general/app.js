import { apiPost, apiGet, API_BASE } from './api.js';
import { buildLayout } from './layout.js';
import { createSidebar } from './sidebar.js';
import { createInfoPanel } from './info_panel.js';
import { createReplayController } from './replay.js';
import { createPipelinePoller } from './pipeline.js';
import { createModal } from './modal.js';
import { playTransition } from './animate.js';

export function createApp(config) {
  const state = {
    sessionId: null,
    gameState: null,
    humanPlayer: 0,
    aiPlayers: [1],
    busy: false,
    forceMode: false,
    hintPending: false,
    lastAiWinrate: null,
    replayMode: false,
    difficulty: null,
  };

  const refs = buildLayout();
  const poller = createPipelinePoller();

  const sidebar = createSidebar(refs.sidebar, config, {
    onStart: startGame,
    onUndo: handleUndo,
    onForce: handleForce,
    onHint: handleHint,
    onLoadReplay: handleLoadReplay,
  });

  const infoPanel = createInfoPanel(refs.infoCol, config);
  const replay = createReplayController(refs.infoCol, config);
  const modal = createModal();

  modal.onReplay(() => enterReplay());
  replay.onExit(() => exitReplay());
  replay.onRenderFrame((frame, allFrames) => {
    if (config.onReplayFrames) config.onReplayFrames(allFrames);
    if (config.renderBoard) {
      config.renderBoard(refs.boardCol, frame, ctx);
    }
    updateReplayInfo(frame);
  });

  const ctx = {
    get state() { return state; },
    get canPlay() {
      if (!state.gameState || state.gameState.is_terminal) return false;
      if (state.busy || poller.isPolling()) return false;
      return !state.aiPlayers.includes(state.gameState.current_player) || state.forceMode;
    },
    submitAction(actionId) { onAction(actionId); },
    rerender() { render(); },
  };

  function render() {
    if (state.replayMode) return;

    if (config.renderBoard) {
      config.renderBoard(refs.boardCol, state.gameState, ctx);
    }
    if (config.renderPlayerArea) {
      config.renderPlayerArea(refs.playerArea, state.gameState, ctx);
    }

    updateInfoPanel();

    if (config.extensions) {
      infoPanel.updateExtensions(state.gameState, config.extensions);
    }
  }

  async function animateTransition(prevState, newState, actionInfo, actionId) {
    if (!config.describeTransition) return;
    try {
      const steps = config.describeTransition(prevState, newState, actionInfo, actionId);
      if (steps && steps.length) await playTransition(steps);
    } catch (e) {
      console.warn('[app] animation failed, falling back to instant render:', e);
    }
  }

  function updateInfoPanel() {
    if (!state.gameState) {
      infoPanel.reset();
      return;
    }

    const gs = state.gameState;

    if (gs.is_terminal) {
      infoPanel.setTurn('对局结束');
      if (gs.winner < 0) {
        infoPanel.setMessage('结果：平局');
      } else if (state.aiPlayers.includes(gs.winner)) {
        infoPanel.setMessage('结果：AI 获胜');
      } else {
        infoPanel.setMessage('结果：你赢了！');
      }
      infoPanel.setWinrate(state.lastAiWinrate);
      showGameOverModal();
      return;
    }

    if (poller.isPolling()) {
      infoPanel.setTurn('当前轮到：AI 思考中...');
    } else if (state.forceMode) {
      infoPanel.setTurn('替对手落子中（请操作）');
    } else if (state.aiPlayers.includes(gs.current_player)) {
      infoPanel.setTurn('当前轮到：AI');
    } else {
      infoPanel.setTurn('当前轮到：你');
    }

    if (gs.last_action_info && config.formatOpponentMove) {
      infoPanel.setMessage(config.formatOpponentMove(gs.last_action_info, gs.last_action_id));
    }

    infoPanel.setWinrate(state.lastAiWinrate);
  }

  function updateReplayInfo(frame) {
    const actor = resolveActorName(frame.actor);
    infoPanel.setTurn('录像回放 · ' + actor);

    let moveText = frame.actor === 'start' ? '开局' : (
      config.formatOpponentMove
        ? config.formatOpponentMove(frame.action_info, frame.action_id)
        : (frame.action_id !== null && frame.action_id !== undefined ? '动作 ' + frame.action_id : '')
    );
    if (frame.is_terminal) moveText += ' · 终局';
    infoPanel.setMessage(moveText);

    const a = frame.analysis;
    if (a) {
      infoPanel.setWinrate(a.best_win_rate);
      if (frame.action_id !== a.best_action && a.best_action_info) {
        const bestText = config.formatSuggestedMove
          ? config.formatSuggestedMove(a.best_action_info, a.best_action)
          : '动作 ' + a.best_action;
        infoPanel.setSuggest(bestText);
      } else {
        infoPanel.setSuggest('最优着法');
      }
    } else {
      infoPanel.setWinrate(null);
      infoPanel.setSuggest(null);
    }
  }

  function showGameOverModal() {
    const gs = state.gameState;
    const showReplay = state.difficulty === 'expert';
    if (gs.winner < 0) {
      modal.show('平局', '本局结束，结果为平局。', showReplay);
    } else if (state.aiPlayers.includes(gs.winner)) {
      modal.show('AI 获胜', 'AI 赢得了本局比赛。', showReplay);
    } else {
      modal.show('你赢了！', '恭喜，你赢得了本局比赛！', showReplay);
    }
  }

  async function startGame(sideMode, difficulty, numPlayers) {
    exitReplay();
    modal.hide();

    numPlayers = numPlayers || 2;
    let humanPlayer;
    if (sideMode === 'random') {
      humanPlayer = Math.floor(Math.random() * numPlayers);
    } else {
      humanPlayer = parseInt(sideMode) || 0;
    }

    const aiPlayers = [];
    for (let i = 0; i < numPlayers; i++) {
      if (i !== humanPlayer) aiPlayers.push(i);
    }

    state.humanPlayer = humanPlayer;
    state.aiPlayers = aiPlayers;
    state.difficulty = difficulty;
    state.forceMode = false;
    state.lastAiWinrate = null;
    state.busy = false;
    poller.cancel();

    try {
      const data = await apiPost(API_BASE, {
        game_id: config.gameId,
        seed: Math.floor(Math.random() * 1000000),
        human_player: humanPlayer,
        num_players: numPlayers,
        difficulty: difficulty,
      });

      state.sessionId = data.session_id;
      state.gameState = data;
      state.humanPlayer = data.human_player;
      state.aiPlayers = data.ai_players;
      if (config.onGameStart) config.onGameStart();
      sidebar.rebuildForceButtons(state.aiPlayers);

      const diffLabels = { heuristic: '启发式', casual: '体验', expert: '专家' };
      const seatLabel = config.getPlayerSymbol ? config.getPlayerSymbol(humanPlayer) : '玩家' + humanPlayer;
      sidebar.setStartMsg('已开局（' + numPlayers + '人），你是' + seatLabel + '，难度=' + (diffLabels[difficulty] || difficulty));
      sidebar.setOpsMsg('');

      render();

      if (state.aiPlayers.includes(state.gameState.current_player)) {
        await apiPost(API_BASE + '/' + state.sessionId + '/ai-action', {});
        await pollPipeline();
      }
    } catch (e) {
      sidebar.setStartMsg(e.message);
    }
  }

  async function onAction(actionId) {
    if (state.busy || state.replayMode || poller.isPolling()) return;
    if (!state.gameState || state.gameState.is_terminal) return;

    const cp = state.gameState.current_player;
    const canAct = !state.aiPlayers.includes(cp) || state.forceMode;
    if (!canAct) return;
    if (!state.gameState.legal_actions.includes(actionId)) return;

    poller.cancel();
    state.hintPending = false;
    infoPanel.setSuggest(null);
    state.busy = true;
    if (config.onActionSubmitted) config.onActionSubmitted();
    try {
      const prevState = state.gameState;
      const data = await apiPost(API_BASE + '/' + state.sessionId + '/action', { action_id: actionId });

      await animateTransition(prevState, data, data.action_info, actionId);

      state.gameState = data;

      if (state.forceMode) {
        state.forceMode = false;
        state.busy = false;
        render();
        infoPanel.setMessage('已完成替对手落子');
        return;
      }

      state.busy = false;
      render();

      if (!state.gameState.is_terminal && state.aiPlayers.includes(state.gameState.current_player)) {
        await pollPipeline();
      }
    } catch (e) {
      sidebar.setOpsMsg('错误：' + e.message);
      state.busy = false;
    }
  }

  function pollOnce() {
    return new Promise(resolve => {
      infoPanel.setTurn('当前轮到：AI 思考中...');
      poller.poll(state.sessionId, {
        onThinking() {
          infoPanel.setTurn('当前轮到：AI 思考中...');
        },
        onAnalysis(analysis) {
          if (analysis && state.difficulty === 'expert') {
            const drop = analysis.drop_score;
            if (drop !== undefined && drop !== null && drop >= 5) {
              const label = drop >= 10 ? '严重失误' : '失误';
              infoPanel.setMessage(label + '：掉分 ' + drop.toFixed(1) + '%');
            }
          }
        },
        onDone(data, aiWinrate, pipeStatus) {
          resolve({
            data,
            aiWinrate,
            aiAction: pipeStatus ? pipeStatus.ai_action : null,
            aiActionInfo: pipeStatus ? pipeStatus.ai_action_info : null,
          });
        },
        onTimeout(data) {
          state.gameState = data;
          sidebar.setOpsMsg('AI 思考超时');
          render();
          resolve(null);
        },
        onError(e) {
          sidebar.setOpsMsg('错误：' + e.message);
          apiGet(API_BASE + '/' + state.sessionId).then(data => {
            state.gameState = data;
            render();
          }).catch(() => {});
          resolve(null);
        },
      });
    });
  }

  async function pollPipeline() {
    while (true) {
      const result = await pollOnce();
      if (!result) break;

      const prevState = state.gameState;
      await animateTransition(prevState, result.data, result.aiActionInfo, result.aiAction);

      state.gameState = result.data;
      if (state.difficulty === 'expert') state.lastAiWinrate = result.aiWinrate;
      render();

      if (state.gameState.is_terminal) break;
      if (!state.aiPlayers.includes(state.gameState.current_player)) break;

      await new Promise(r => setTimeout(r, 200));
      await apiPost(API_BASE + '/' + state.sessionId + '/ai-action', {}).catch(() => {});
    }
  }

  async function handleUndo() {
    if (!state.sessionId || state.busy || state.replayMode) return;
    if (state.gameState.is_terminal) {
      sidebar.setOpsMsg('对局已结束，无法悔棋');
      return;
    }

    poller.cancel();
    state.hintPending = false;
    infoPanel.setSuggest(null);
    state.busy = true;
    sidebar.setOpsMsg('');
    try {
      const humanTag = 'player_' + state.humanPlayer;
      let attempts = 0;
      while (attempts < 20) {
        const data = await apiPost(API_BASE + '/' + state.sessionId + '/step-back', {});
        state.gameState = data;
        attempts++;
        if (!data.legal_actions.length) break;
        if (state.aiPlayers.includes(data.current_player)) continue;
        if (data.last_actor === humanTag) continue;
        break;
      }
      state.forceMode = false;
      state.lastAiWinrate = null;
      if (config.onUndo) config.onUndo();
      state.busy = false;
      render();
      infoPanel.setMessage('已悔棋');
    } catch (e) {
      sidebar.setOpsMsg(e.message);
      state.busy = false;
    }
  }

  async function handleForce(targetPlayer) {
    if (!state.sessionId || state.busy || state.replayMode) return;
    if (state.gameState.is_terminal) {
      sidebar.setOpsMsg('对局已结束');
      return;
    }

    poller.cancel();
    state.hintPending = false;
    infoPanel.setSuggest(null);
    state.busy = true;
    sidebar.setOpsMsg('');
    try {
      let attempts = 0;
      while (attempts < 20) {
        const data = await apiPost(API_BASE + '/' + state.sessionId + '/step-back', {});
        state.gameState = data;
        attempts++;
        if (!data.legal_actions.length) break;
        const isTarget = targetPlayer !== undefined
          ? data.current_player === targetPlayer
          : state.aiPlayers.includes(data.current_player);
        if (!isTarget) continue;
        const actorTag = 'player_' + data.current_player;
        if (data.last_actor === actorTag) continue;
        break;
      }
      const cp = state.gameState.current_player;
      const reached = targetPlayer !== undefined ? cp === targetPlayer : state.aiPlayers.includes(cp);
      if (reached) {
        state.forceMode = true;
        state.busy = false;
        render();
        const name = config.getPlayerSymbol ? config.getPlayerSymbol(cp) : '玩家' + cp;
        infoPanel.setMessage('请替' + name + '落子');
      } else {
        state.busy = false;
        sidebar.setOpsMsg('无法回退到该对手的回合');
      }
    } catch (e) {
      sidebar.setOpsMsg(e.message);
      state.busy = false;
    }
  }

  async function handleHint() {
    if (!state.sessionId || state.busy || state.replayMode) return;
    if (state.gameState.is_terminal) {
      sidebar.setOpsMsg('对局已结束');
      return;
    }
    if (state.hintPending || poller.isPolling()) return;

    state.hintPending = true;
    infoPanel.setSuggest('局面分析中...');
    try {
      const data = await apiPost(API_BASE + '/' + state.sessionId + '/ai-hint', {});
      if (!state.hintPending) return;
      const text = config.formatSuggestedMove
        ? config.formatSuggestedMove(data.action_info, data.action)
        : '动作 ' + data.action;
      infoPanel.setSuggest(text);
    } catch (e) {
      if (!state.hintPending) return;
      infoPanel.setSuggest('--');
      sidebar.setOpsMsg(e.message);
    }
    state.hintPending = false;
  }

  async function enterReplay() {
    if (!state.sessionId) return;
    modal.hide();
    try {
      state.replayMode = true;
      replayMeta = null;
      replay.setResolveActor(resolveActorName);
      const data = await replay.enter(state.sessionId);
      replayMeta = extractPlayersMeta(data);
    } catch (e) {
      sidebar.setOpsMsg('加载录像失败：' + e.message);
      state.replayMode = false;
    }
  }

  function exitReplay() {
    if (!state.replayMode) return;
    replay.exit();
    state.replayMode = false;
    render();
  }

  let replayMeta = null;

  function resolveActorName(actor) {
    if (actor === 'start') return '开局';
    if (replayMeta) {
      const info = replayMeta[actor];
      if (info) return info.name || actor;
    }
    return actor;
  }

  function extractPlayersMeta(data) {
    if (data.players) return data.players;
    if (data.player_0 && typeof data.player_0 === 'string') {
      return {
        player_0: { name: data.player_0, type: 'unknown' },
        player_1: { name: data.player_1, type: 'unknown' },
      };
    }
    return null;
  }

  function buildReplayHeader(meta) {
    if (!meta) return '录像回放';
    const names = [];
    for (let i = 0; ; i++) {
      const key = 'player_' + i;
      if (!meta[key]) break;
      names.push(meta[key].name || key);
    }
    return names.length > 0 ? names.join(' vs ') : '录像回放';
  }

  async function handleLoadReplay(data, error) {
    if (error) {
      sidebar.setOpsMsg(error);
      return;
    }

    replayMeta = extractPlayersMeta(data);
    replay.setResolveActor(resolveActorName);

    const header = buildReplayHeader(replayMeta);
    sidebar.setOpsMsg(header);

    if (data.frames) {
      modal.hide();
      state.replayMode = true;
      replay.enterWithFrames(data.frames);
    } else if (data.action_history) {
      sidebar.setOpsMsg(header + '（生成帧中…）');
      try {
        const resp = await fetch('/api/replay/build', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            game_id: data.game_id,
            seed: data.seed,
            action_history: data.action_history,
          }),
        });
        if (!resp.ok) throw new Error('生成帧失败: ' + resp.status);
        const built = await resp.json();
        modal.hide();
        state.replayMode = true;
        replay.enterWithFrames(built.frames);
        sidebar.setOpsMsg(header);
      } catch (e) {
        sidebar.setOpsMsg('生成帧失败：' + e.message);
      }
    } else {
      sidebar.setOpsMsg('录像文件格式错误：缺少 frames 或 action_history');
    }
  }

  render();
}
