const ZOOM_KEY = 'dino_board_zoom';

function injectZoomControls(target) {
  const el = document.createElement('div');
  el.className = 'zoom-controls';
  el.innerHTML =
    '<button class="zoom-btn" data-zoom="out" type="button" title="缩小">−</button>' +
    '<span class="zoom-value">100%</span>' +
    '<button class="zoom-btn" data-zoom="in" type="button" title="放大">+</button>';
  document.body.appendChild(el);

  let zoom = parseInt(localStorage.getItem(ZOOM_KEY) || '100', 10);
  const valueEl = el.querySelector('.zoom-value');

  function apply() {
    target.style.transform = 'scale(' + (zoom / 100) + ')';
    valueEl.textContent = zoom + '%';
    localStorage.setItem(ZOOM_KEY, String(zoom));
  }
  apply();

  el.addEventListener('click', (e) => {
    const btn = e.target.closest('[data-zoom]');
    if (!btn) return;
    if (btn.dataset.zoom === 'in') zoom = Math.min(180, zoom + 10);
    else zoom = Math.max(60, zoom - 10);
    apply();
  });
}

export function buildLayout() {
  document.body.innerHTML = '';

  const main = document.createElement('main');
  main.className = 'layout';

  const sidebar = document.createElement('aside');
  sidebar.className = 'sidebar';

  const stage = document.createElement('section');
  stage.className = 'board-stage';
  stage.id = 'board-stage';

  const content = document.createElement('div');
  content.className = 'board-content';
  content.id = 'board-content';

  const tableArea = document.createElement('div');
  tableArea.className = 'table-area';

  const boardCol = document.createElement('div');
  boardCol.className = 'game-board-col';
  boardCol.id = 'play-area';

  const infoCol = document.createElement('div');
  infoCol.className = 'info-col';

  tableArea.appendChild(boardCol);
  tableArea.appendChild(infoCol);

  const playerArea = document.createElement('div');
  playerArea.className = 'player-area';
  playerArea.id = 'player-area';

  content.appendChild(tableArea);
  content.appendChild(playerArea);

  stage.appendChild(content);

  main.appendChild(sidebar);
  main.appendChild(stage);
  document.body.appendChild(main);

  const toggle = document.createElement('button');
  toggle.className = 'sidebar-toggle';
  toggle.type = 'button';
  toggle.title = '收起/展开侧边栏';
  toggle.textContent = '◀';
  document.body.appendChild(toggle);

  toggle.addEventListener('click', () => {
    const collapsed = main.classList.toggle('sidebar-collapsed');
    toggle.classList.toggle('collapsed', collapsed);
    toggle.textContent = collapsed ? '▶' : '◀';
  });

  injectZoomControls(content);

  return { sidebar, boardCol, infoCol, playerArea, stage };
}
