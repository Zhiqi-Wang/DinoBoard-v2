import { apiGet, API_BASE } from './api.js';

export function createPipelinePoller() {
  let polling = false;
  let cancelled = false;

  async function poll(sessionId, callbacks) {
    polling = true;
    cancelled = false;
    const started = Date.now();
    let analysisDelivered = false;

    try {
      while (Date.now() - started < 45000) {
        if (cancelled) { polling = false; return; }

        const st = await apiGet(API_BASE + '/' + sessionId + '/pipeline');
        const phase = st.phase || '';

        if (!analysisDelivered && st.analysis && callbacks.onAnalysis) {
          callbacks.onAnalysis(st.analysis);
          analysisDelivered = true;
        }

        if (phase === 'analyzing' || phase === 'ai_thinking' || phase === 'queued') {
          if (callbacks.onThinking) callbacks.onThinking();
        }

        if (phase === 'done' || phase === 'idle') {
          if (cancelled) { polling = false; return; }
          if (!analysisDelivered && st.analysis && callbacks.onAnalysis) {
            callbacks.onAnalysis(st.analysis);
          }
          const data = await apiGet(API_BASE + '/' + sessionId);
          if (cancelled) { polling = false; return; }
          // The C++ NetMctsStats field is best_action_value, but the
          // pybind export renames it to best_value (see py_engine.cpp
          // sample dict keys). Don't use the C++ field name here — same
          // trap bit us in pipeline.py (BUG-020). Always key off the
          // Python-side name.
          let aiWinrate = null;
          if (st.ai_stats && st.ai_stats.best_value !== undefined) {
            aiWinrate = (st.ai_stats.best_value + 1) / 2;
          }
          polling = false;
          if (callbacks.onDone) callbacks.onDone(data, aiWinrate, st);
          return;
        }

        await new Promise(r => setTimeout(r, 300));
      }

      polling = false;
      const data = await apiGet(API_BASE + '/' + sessionId);
      if (callbacks.onTimeout) callbacks.onTimeout(data);
    } catch (e) {
      polling = false;
      if (callbacks.onError) callbacks.onError(e);
    }
  }

  return {
    poll,
    cancel() { cancelled = true; polling = false; },
    isPolling() { return polling; },
  };
}
