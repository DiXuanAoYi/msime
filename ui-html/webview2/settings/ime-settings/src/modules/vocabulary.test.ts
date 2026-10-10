import { beforeEach, afterEach, expect, it, vi } from 'vitest';
const callbacks = vi.hoisted(() => new Set<(data: any) => void>());
vi.mock('../utils/host-messages', () => ({ onHostMessage: (_: string, fn: (data: any) => void) => { callbacks.add(fn); } }));
class Node {
  dataset: Record<string, string> = {};
  textContent = '';
  children: Node[] = [];
  hidden = true;
  value = '90';
  checked = false;
  handlers = new Map<string, (event?: any) => void>();
  addEventListener(name: string, fn: (event?: any) => void) { this.handlers.set(name, fn); }
  click() { this.handlers.get('click')?.(); }
  append(child: Node) { this.children.push(child); }
  replaceChildren() { this.children = []; }
}
let nodes: Map<string, Node>;
let post: ReturnType<typeof vi.fn>;
let module: typeof import('./vocabulary');
const get = (id: string) => nodes.get(id)!;
beforeEach(async () => {
  vi.resetModules(); vi.useFakeTimers(); callbacks.clear();
  nodes = new Map(['vocabularyPanel', 'vocabularyEnabled', 'vocabularyRetention', 'vocabularyStatus',
    'vocabularyRefresh', 'vocabularyExport', 'vocabularyClear', 'vocabularyConfirm', 'vocabularyCancelClear',
    'vocabularyConfirmClear', 'vocabularySummary', 'vocabularyRows'].map(id => [id, new Node()]));
  post = vi.fn();
  vi.stubGlobal('document', { getElementById: (id: string) => nodes.get(id), createElement: () => new Node() });
  vi.stubGlobal('window', { chrome: { webview: { postMessage: post } }, setTimeout, clearTimeout });
  module = await import('./vocabulary'); module.setupVocabulary(vi.fn());
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
it('requires three observed commit rounds for familiarity', () => {
  expect(module.familiarity(2)).toBe('生词 2/3'); expect(module.familiarity(3)).toBe('已熟悉');
});
it('ignores stale responses and renders vocabulary as text', () => {
  get('vocabularyRefresh').click();
  const sent = JSON.parse(post.mock.calls[0][0]);
  const response = { requestId: 'stale', action: 'overview', ok: true, rows: [], total: 9 };
  callbacks.forEach(fn => fn(response)); expect(get('vocabularySummary').textContent).toBe('');
  callbacks.forEach(fn => fn({ ...response, requestId: sent.data.requestId,
    rows: [{ language: 'en', word: '<img src=x>', seen: 3, selected: 1, used: 0 }] }));
  expect(get('vocabularyRows').children[0].children[1].textContent).toBe('<img src=x>');
  expect(get('vocabularySummary').textContent).toContain('9');
});
it('requires the explicit second clear action and allows cancellation', () => {
  get('vocabularyClear').click(); expect(post).not.toHaveBeenCalled();
  expect(get('vocabularyConfirm').hidden).toBe(false);
  get('vocabularyCancelClear').click(); expect(post).not.toHaveBeenCalled();
  expect(get('vocabularyConfirm').hidden).toBe(true);
  get('vocabularyClear').click(); get('vocabularyConfirmClear').click();
  expect(JSON.parse(post.mock.calls[0][0]).data.action).toBe('clear');
});
it('recovers from timeout without claiming that an operation succeeded', () => {
  get('vocabularyRefresh').click(); vi.advanceTimersByTime(5001);
  expect(get('vocabularyStatus').textContent).toContain('超时');
  get('vocabularyRefresh').click(); expect(post).toHaveBeenCalledTimes(2);
});
