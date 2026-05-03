(function () {
  var ZOOM_KEY = 'dino_board_zoom';

  function injectZoomControls() {
    var target = document.getElementById('board-content') || document.getElementById('board-stage');
    if (!target) return;

    var el = document.createElement('div');
    el.className = 'zoom-controls';
    el.id = 'zoom-controls';
    el.innerHTML =
      '<button id="btn-zoom-out" class="zoom-btn" type="button" title="缩小">-</button>' +
      '<span id="zoom-value" class="zoom-value">100%</span>' +
      '<button id="btn-zoom-in" class="zoom-btn" type="button" title="放大">+</button>';
    document.body.appendChild(el);

    var zoom = parseInt(localStorage.getItem(ZOOM_KEY) || '100', 10);

    function apply() {
      target.style.transform = 'scale(' + (zoom / 100) + ')';
      target.style.transformOrigin = 'top center';
      document.getElementById('zoom-value').textContent = zoom + '%';
      localStorage.setItem(ZOOM_KEY, String(zoom));
    }
    apply();

    document.getElementById('btn-zoom-in').addEventListener('click', function () {
      zoom = Math.min(180, zoom + 10);
      apply();
    });
    document.getElementById('btn-zoom-out').addEventListener('click', function () {
      zoom = Math.max(60, zoom - 10);
      apply();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', injectZoomControls);
  } else {
    injectZoomControls();
  }
})();
