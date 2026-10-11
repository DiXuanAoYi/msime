import { afterEach, expect, it, vi } from 'vitest';
vi.mock('../utils/host-messages', () => ({ onHostMessage: vi.fn() }));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./local-tts', () => ({ setupLocalTts: vi.fn() }));
import { setupSpeech } from './speech';

afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
it('polls an initially open speech panel so asynchronous synthesis results become visible', () => {
  vi.useFakeTimers();
  const events = new Map<string, () => void>();
  const panel = { open: true, addEventListener: (name: string, fn: () => void) => events.set(name, fn) };
  const provider = { dataset: {}, add: vi.fn(), addEventListener: vi.fn(), value: '' };
  const sent: any[] = [];
  vi.stubGlobal('Option', class {});
  vi.stubGlobal('document', { hidden: false, getElementById: (id: string) => id === 'speechPanel' ? panel : id === 'speech-provider' ? provider : null });
  vi.stubGlobal('window', { setInterval, addEventListener: vi.fn(), chrome: { webview: { postMessage: (message: string) => sent.push(JSON.parse(message)) } } });
  setupSpeech();
  expect(sent.at(-1)).toMatchObject({ type: 'speechRequest', data: { action: 'status' } });
  vi.advanceTimersByTime(2000);
  expect(sent).toHaveLength(2);
  panel.open = false; events.get('toggle')!();
  vi.advanceTimersByTime(4000);
  expect(sent).toHaveLength(2);
});
