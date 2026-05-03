/**
 * General-purpose animation engine for game transitions.
 *
 * Games provide describeTransition(prevState, newState, actionInfo, actionId)
 * which returns an array of animation steps. This module executes them on the
 * current DOM (still showing the old state), then the caller re-renders.
 *
 * If ANY step fails (bad selector, createElement throws, etc.), it is silently
 * skipped. If the whole describeTransition throws, the caller catches it and
 * falls back to instant render. The game never freezes due to animation bugs.
 *
 * Step types:
 *   fly       — move a floating element from one position to another
 *   fadeOut   — fade an existing DOM element to transparent
 *   highlight — briefly add a CSS class to an element
 *   pause     — wait for a duration (for sequencing)
 *
 * fly step fields:
 *   from         — CSS selector or HTMLElement (start position)
 *   to           — CSS selector or HTMLElement (end position)
 *   createElement — () => HTMLElement (visual for the flying object)
 *   duration     — ms (default 350)
 *   hideFrom     — if true, source element becomes invisible after flight
 *                  (keeps layout; next steps see "it's gone")
 *   onComplete   — optional callback after flight finishes, for updating
 *                  DOM text (e.g. decrementing a counter) so subsequent
 *                  steps see the intermediate visual state
 */

const DEFAULT_DURATION = 350;
const STEP_GAP = 60;
const MAX_QUEUE_MS = 5000;

export async function playTransition(steps) {
  if (!steps || !steps.length) return;

  const overlay = document.createElement('div');
  overlay.className = 'anim-overlay';
  document.body.appendChild(overlay);

  const hidden = [];
  const deadline = Date.now() + MAX_QUEUE_MS;
  try {
    for (let i = 0; i < steps.length; i++) {
      if (Date.now() > deadline) break;
      try {
        await executeStep(overlay, steps[i], hidden);
      } catch (e) {
        console.warn('[anim] step skipped:', e);
      }
      if (i < steps.length - 1) await sleep(STEP_GAP);
    }
  } finally {
    // Restore visibility of all hidden elements before caller re-renders
    // (the re-render will replace the DOM anyway, but be safe)
    for (const el of hidden) {
      el.style.visibility = '';
    }
    overlay.remove();
  }
}

function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

function resolveEl(ref) {
  if (!ref) return null;
  if (typeof ref === 'string') return document.querySelector(ref);
  if (ref instanceof HTMLElement) return ref;
  return null;
}

function getRect(ref) {
  const el = resolveEl(ref);
  if (!el) return null;
  return el.getBoundingClientRect();
}

async function executeStep(overlay, step, hidden) {
  if (!step || !step.type) return;
  switch (step.type) {
    case 'fly': return stepFly(overlay, step, hidden);
    case 'fadeOut': return stepFadeOut(step);
    case 'highlight': return stepHighlight(step);
    case 'pause': return sleep(step.duration || 200);
  }
}

async function stepFly(overlay, step, hidden) {
  const fromRect = getRect(step.from);
  const toRect = getRect(step.to);
  if (!fromRect || !toRect) return;

  const dur = step.duration || DEFAULT_DURATION;

  let flyer;
  if (step.createElement) {
    flyer = step.createElement();
  } else {
    const srcEl = resolveEl(step.from);
    flyer = srcEl ? srcEl.cloneNode(true) : null;
  }
  if (!flyer) return;

  Object.assign(flyer.style, {
    position: 'fixed',
    left: fromRect.left + 'px',
    top: fromRect.top + 'px',
    width: fromRect.width + 'px',
    height: fromRect.height + 'px',
    transition: `left ${dur}ms ease-in-out, top ${dur}ms ease-in-out`,
    pointerEvents: 'none',
    zIndex: '10001',
    margin: '0',
  });
  overlay.appendChild(flyer);

  await new Promise(resolve => {
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        flyer.style.left = toRect.left + 'px';
        flyer.style.top = toRect.top + 'px';
        resolve();
      });
    });
  });

  await sleep(dur + 20);
  flyer.remove();

  if (step.hideFrom) {
    const srcEl = resolveEl(step.from);
    if (srcEl) {
      srcEl.style.visibility = 'hidden';
      hidden.push(srcEl);
    }
  }

  if (step.onComplete) {
    try { step.onComplete(); } catch (e) { console.warn('[anim] onComplete failed:', e); }
  }
}

async function stepFadeOut(step) {
  const el = resolveEl(step.target);
  if (!el) return;
  const dur = step.duration || DEFAULT_DURATION;
  el.style.transition = `opacity ${dur}ms ease`;
  el.style.opacity = '0';
  await sleep(dur);
}

async function stepHighlight(step) {
  const el = resolveEl(step.target);
  if (!el) return;
  const dur = step.duration || 600;
  const cls = step.className || 'anim-highlight';
  el.classList.add(cls);
  await sleep(dur);
  el.classList.remove(cls);
}
