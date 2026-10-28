// 语音输入页同时放识别和润色两块，所以一个标签可以对应多个面板（data-voice-pane 用空格分隔）。
const panes = ['input', 'tts', 'studio', 'models'] as const;
export function activateVoicePane(name: string): void {
  const root = document.getElementById('voice-settings');
  if (!root || !panes.includes(name as typeof panes[number])) return;
  for (const pane of root.querySelectorAll<HTMLElement>('[data-voice-pane]'))
    pane.hidden = !(pane.dataset.voicePane ?? '').split(' ').includes(name);
  for (const tab of root.querySelectorAll<HTMLElement>('[data-voice-target]')) {
    const selected = tab.dataset.voiceTarget === name;
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
  }
  root.dataset.voicePane = name;
  if (name === 'studio') {
    const panel = document.getElementById('voiceStudioPanel') as HTMLDetailsElement | null;
    if (panel) panel.open = true;
  }
}
export function setupVoiceNavigation(): void {
  const root = document.getElementById('voice-settings');
  if (!root || root.dataset.navigationReady) return;
  root.dataset.navigationReady = '1';
  const tabs = [...root.querySelectorAll<HTMLButtonElement>('[data-voice-target]')];
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => activateVoicePane(tab.dataset.voiceTarget ?? 'input'));
    tab.addEventListener('keydown', (event) => {
      const offset = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : 0;
      if (!offset) return;
      event.preventDefault();
      const target = tabs[(index + offset + tabs.length) % tabs.length];
      activateVoicePane(target.dataset.voiceTarget ?? 'input'); target.focus();
    });
  });
  activateVoicePane(root.dataset.voicePane ?? 'input');
}
