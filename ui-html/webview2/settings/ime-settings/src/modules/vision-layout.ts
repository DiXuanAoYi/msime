/** Move the existing source editor between workflows; its value and handlers stay intact. */
export function setupVisionLayout(): void {
  const root = document.getElementById('visionPanel');
  if (!root || root.dataset.layoutReady) return;
  root.dataset.layoutReady = '1';
  const tabs = Array.from(root.querySelectorAll<HTMLButtonElement>('[data-vision-tab]'));
  const editor = document.getElementById('visionSourceEditor');
  const services = document.getElementById('visionServiceBar');
  const activate = (tab: HTMLButtonElement, focus = false) => {
    const name = tab.dataset.visionTab ?? 'image';
    for (const button of tabs) {
      const selected = button === tab;
      button.setAttribute('aria-selected', String(selected));
      button.tabIndex = selected ? 0 : -1;
    }
    root.querySelectorAll<HTMLElement>('[data-vision-pane]').forEach(pane => {
      pane.hidden = pane.dataset.visionPane !== name;
    });
    if (services) services.hidden = name === 'image' || name === 'services';
    if (editor && (name === 'image' || name === 'text')) {
      root.querySelector(`[data-vision-source-host="${name}"]`)?.append(editor);
    }
    window.dispatchEvent(new CustomEvent('msime-vision-pane-change', { detail: name }));
    if (focus) tab.focus();
  };
  for (const tab of tabs) {
    tab.addEventListener('click', () => activate(tab));
    tab.addEventListener('keydown', event => {
      const index = tabs.indexOf(tab);
      const next = event.key === 'ArrowRight' ? (index + 1) % tabs.length
        : event.key === 'ArrowLeft' ? (index + tabs.length - 1) % tabs.length
        : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1;
      if (next < 0) return;
      event.preventDefault();
      activate(tabs[next], true);
    });
  }
  if (tabs[0]) activate(tabs[0]);
}
