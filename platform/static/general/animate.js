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
 *   flyGroup  — run multiple fly sub-flights in PARALLEL (all start and
 *               end together, same total duration as a single fly)
 *   group     — run arbitrary child steps in PARALLEL (generalizes
 *               flyGroup to any mix of fly/popup/highlight)
 *   popup     — show a floating text or element at a target location that
 *               floats up and fades out (e.g. "+5" score pops on a wall
 *               cell). Target rect is captured when the step starts; the
 *               target's later re-render doesn't move the popup.
 *   run       — invoke an arbitrary callback to mutate the DOM between
 *               other animation steps (e.g. show an intermediate "pattern
 *               row filled" state in Azul's round-end settlement before
 *               the subsequent pattern→wall flights start).
 *   fadeOut   — fade an existing DOM element to transparent
 *   highlight — briefly add a CSS class to an element
 *   pause     — wait for a duration (for sequencing)
 *
 * fly step fields:
 *   from         — CSS selector or HTMLElement (start position)
 *   to           — CSS selector or HTMLElement (end position)
 *   createElement — () => HTMLElement (visual for the flying object)
 *   duration     — ms (default 350)
 *   width/height — optional explicit size for the flying element. When
 *                  omitted, the source element's rect size is used. Use
 *                  this when `from` resolves to a container bigger than
 *                  the logical "thing flying" (e.g. a factory disc vs. a
 *                  single tile).
 *   onStart(srcEl) — optional callback invoked AFTER the source rect is
 *                  captured and the flying sprite is created, but
 *                  BEFORE the sprite animates. Use this to transform
 *                  the source DOM into its "post-take" representation
 *                  (e.g. show an empty-slot ghost where the tile was).
 *                  Preferred over `hideFrom: true` when the source is a
 *                  container with a meaningful empty-state background
 *                  — hiding the whole container also hides that
 *                  background, which users read as "the slot broke"
 *                  rather than "the tile was taken". (See Web design
 *                  principles doc for the "don't hide a container to
 *                  hide its contents" rule.)
 *   hideFrom     — if true, source element becomes invisible for the
 *                  rest of the transition (visibility:hidden, keeps
 *                  layout). Simpler than onStart but hides the whole
 *                  source including any empty-state background. Use
 *                  only when the source has no meaningful empty state
 *                  to preserve (e.g. a specific tile with no ghost
 *                  backing, or a factory disc whose visible-empty
 *                  state naturally appears at re-render).
 *   onComplete   — optional callback after flight finishes, for updating
 *                  DOM text (e.g. decrementing a counter) so subsequent
 *                  steps see the intermediate visual state
 *
 * flyGroup step fields:
 *   flights       — array of fly-step objects (without the `type` field).
 *                   All are launched simultaneously and awaited together.
 *   The flyGroup itself has no duration; it finishes when the slowest
 *   flight finishes.
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
    case 'flyGroup': return stepFlyGroup(overlay, step, hidden);
    case 'group': return stepGroup(overlay, step, hidden);
    case 'popup': return stepPopup(overlay, step);
    case 'run': return stepRun(step);
    case 'fadeOut': return stepFadeOut(step);
    case 'highlight': return stepHighlight(step);
    case 'pause': return sleep(step.duration || 200);
  }
}

async function stepRun(step) {
  if (typeof step.fn === 'function') step.fn();
}

async function stepFlyGroup(overlay, step, hidden) {
  if (!step.flights || !step.flights.length) return;
  await Promise.all(
    step.flights.map(flight =>
      stepFly(overlay, flight, hidden).catch(e => {
        console.warn('[anim] flight in group skipped:', e);
      })
    )
  );
}

async function stepGroup(overlay, step, hidden) {
  if (!step.children || !step.children.length) return;
  await Promise.all(
    step.children.map(child =>
      executeStep(overlay, child, hidden).catch(e => {
        console.warn('[anim] group child skipped:', e);
      })
    )
  );
}

async function stepPopup(overlay, step) {
  const rect = getRect(step.target);
  if (!rect) return;
  const dur = step.duration || 900;

  const el = document.createElement('div');
  el.className = 'anim-popup' + (step.className ? (' ' + step.className) : '');
  if (step.content instanceof HTMLElement) {
    el.appendChild(step.content);
  } else {
    el.textContent = String(step.content != null ? step.content : '');
  }

  const w = step.width || 60;
  const h = step.height || 28;
  Object.assign(el.style, {
    position: 'fixed',
    left: (rect.left + rect.width / 2 - w / 2) + 'px',
    top: (rect.top + rect.height / 2 - h / 2) + 'px',
    width: w + 'px',
    height: h + 'px',
    pointerEvents: 'none',
    zIndex: '10002',
    // Driven by the anim-popup keyframes in common.css.
    animationDuration: dur + 'ms',
  });
  overlay.appendChild(el);
  await sleep(dur);
  el.remove();
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

  // Size: caller-specified width/height wins (for when `from` is a
  // bigger container than the logical thing flying). Otherwise inherit
  // from source rect so the default case still works for Splendor etc.
  const w = step.width != null ? step.width : fromRect.width;
  const h = step.height != null ? step.height : fromRect.height;
  // Center the flyer on the source/destination rects rather than
  // top-left-aligning. Matters when the flyer is smaller than the rect.
  const startLeft = fromRect.left + fromRect.width / 2 - w / 2;
  const startTop = fromRect.top + fromRect.height / 2 - h / 2;
  const endLeft = toRect.left + toRect.width / 2 - w / 2;
  const endTop = toRect.top + toRect.height / 2 - h / 2;

  Object.assign(flyer.style, {
    position: 'fixed',
    left: startLeft + 'px',
    top: startTop + 'px',
    width: w + 'px',
    height: h + 'px',
    transition: `left ${dur}ms ease-in-out, top ${dur}ms ease-in-out`,
    pointerEvents: 'none',
    zIndex: '10001',
    margin: '0',
  });
  overlay.appendChild(flyer);

  // Hide the source NOW (not after the flight) so the animation reads as
  // "this thing moved there" rather than "a copy flew while the original
  // stayed, then popped out". We capture the source rect above before
  // hiding so the flight still starts at the correct position.
  const srcEl = resolveEl(step.from);
  if (step.onStart && srcEl) {
    try { step.onStart(srcEl); } catch (e) { console.warn('[anim] onStart failed:', e); }
  }
  if (step.hideFrom && srcEl) {
    srcEl.style.visibility = 'hidden';
    hidden.push(srcEl);
  }

  await new Promise(resolve => {
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        flyer.style.left = endLeft + 'px';
        flyer.style.top = endTop + 'px';
        resolve();
      });
    });
  });

  await sleep(dur + 20);
  flyer.remove();

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
