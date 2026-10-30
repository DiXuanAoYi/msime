// Decoration-only interaction. Never focus the host or post an input/commit message.
(() => {
  let pressed = false, timer = 0;
  const root = () => document.getElementById('realContainerParent');
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  function box() {
    const node = root(); if (!node || reduced.matches) return null;
    const css = getComputedStyle(document.documentElement);
    const width = Number.parseFloat(css.getPropertyValue('--msime-skin-decoration-width')) || 0;
    const height = Number.parseFloat(css.getPropertyValue('--msime-skin-decoration-top')) || 0;
    if (!width || !height || !node.querySelector('.cand')) return null;
    const rect = node.getBoundingClientRect();
    const alignment = Number.parseFloat(css.getPropertyValue('--msime-skin-decoration-align')) || 0;
    return {x: rect.left + (rect.width - width) * alignment / 2, y: rect.top, width, height};
  }
  function hit(event) {
    const rect = box();
    return rect && event.clientX >= rect.x && event.clientX <= rect.x + rect.width &&
      event.clientY >= rect.y && event.clientY <= rect.y + rect.height ? rect : null;
  }
  function pose(angle = 0, lift = 0, scale = 1) {
    const node = root(); if (!node) return;
    node.style.setProperty('--msime-decoration-angle', `${angle}deg`);
    node.style.setProperty('--msime-decoration-lift', `${lift}px`);
    node.style.setProperty('--msime-decoration-scale', String(scale));
  }
  function reset() { pressed = false; cancelAnimationFrame(timer); timer = 0; pose(); }
  document.addEventListener('mousemove', event => {
    if (timer) return;
    const rect = hit(event);
    pose(rect ? 2 * (2 * (event.clientX - rect.x) / rect.width - 1) : 0, rect ? -1 : 0);
  });
  document.addEventListener('mouseleave', reset);
  document.addEventListener('mousedown', event => {
    pressed = event.button === 0 && !!hit(event);
    if (pressed) { event.preventDefault(); event.stopImmediatePropagation(); }
  }, true);
  document.addEventListener('click', event => {
    const wasPressed = pressed; pressed = false;
    if (!wasPressed || !hit(event)) return;
    event.preventDefault(); event.stopImmediatePropagation();
    cancelAnimationFrame(timer);
    const start = performance.now();
    function frame(now) {
      const phase = Math.min(1, (now - start) / 320);
      if (!box() || phase === 1) { timer = 0; pose(); return; }
      const pulse = Math.sin(phase * Math.PI);
      pose(2 * Math.sin(phase * Math.PI * 2) * (1 - phase), -3 * pulse, 1 + 0.025 * pulse);
      timer = requestAnimationFrame(frame);
    }
    timer = requestAnimationFrame(frame);
  }, true);
  reduced.addEventListener('change', reset);
  window.addEventListener('blur', reset);
})();
