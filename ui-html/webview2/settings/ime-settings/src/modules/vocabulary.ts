import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';

export function familiarity(seen: number): string { return seen >= 3 ? '已熟悉' : `生词 ${seen}/3`; }
const element = <T extends HTMLElement>(id: string) => document.getElementById(id) as T | null;
let requestCounter = 0;
let waiting = '';
let timer: number | undefined;
const status = (text: string) => { const node = element('vocabularyStatus'); if (node) node.textContent = text; };

export function applyVocabularyConfig(config: Record<string, unknown> | undefined): void {
  const enabled = element<HTMLInputElement>('vocabularyEnabled');
  const retention = element<HTMLSelectElement>('vocabularyRetention');
  if (enabled && typeof config?.enabled === 'boolean') enabled.checked = config.enabled;
  if (retention && [30, 90, 365].includes(Number(config?.retention_days))) retention.value = String(config?.retention_days);
}

export function setupVocabulary(update: (path: string, value: boolean | number) => void): void {
  const root = element('vocabularyPanel');
  if (!root || root.dataset.initialized) return;
  root.dataset.initialized = 'true';
  element<HTMLInputElement>('vocabularyEnabled')?.addEventListener('change', event => {
    update('vocabulary.enabled', (event.target as HTMLInputElement).checked);
  });
  element<HTMLSelectElement>('vocabularyRetention')?.addEventListener('change', event => {
    update('vocabulary.retention_days', Number((event.target as HTMLSelectElement).value));
  });
  function request(action: 'overview' | 'export' | 'clear'): void {
    if (waiting) return;
    if (!window.chrome?.webview) { status('请在知语设置窗口中查看本机记录。'); return; }
    waiting = `vocabulary-${Date.now()}-${++requestCounter}`;
    window.chrome.webview.postMessage(serializeHostMessage({ type: 'vocabularyRequest', data: { requestId: waiting, action } }));
    status('正在处理…');
    timer = window.setTimeout(() => { waiting = ''; status('请求超时，请重试。'); }, 5000);
  }
  element('vocabularyRefresh')?.addEventListener('click', () => request('overview'));
  element('vocabularyExport')?.addEventListener('click', () => request('export'));
  element('vocabularyClear')?.addEventListener('click', () => { const n = element('vocabularyConfirm'); if (n) n.hidden = false; });
  element('vocabularyCancelClear')?.addEventListener('click', () => { const n = element('vocabularyConfirm'); if (n) n.hidden = true; });
  element('vocabularyConfirmClear')?.addEventListener('click', () => {
    const n = element('vocabularyConfirm'); if (n) n.hidden = true;
    request('clear');
  });
  onHostMessage('vocabularyResponse', message => {
    if (message.requestId !== waiting) return;
    window.clearTimeout(timer); waiting = '';
    if (!message.ok) { status(message.message); return; }
    const summary = element('vocabularySummary');
    if (summary) summary.textContent = `已见 ${message.total ?? 0} 词 · 熟悉 ${message.familiar ?? 0} 词 · 选过 ${message.selected ?? 0} 词 · 直接用过 ${message.used ?? 0} 词`;
    const body = element('vocabularyRows');
    if (body) {
      body.replaceChildren();
      for (const row of message.rows) {
        const tr = document.createElement('tr');
        for (const text of [row.language, row.word, familiarity(row.seen), String(row.selected), String(row.used)]) {
          const td = document.createElement('td'); td.textContent = text; tr.append(td);
        }
        body.append(tr);
      }
    }
    if (message.action === 'export' && message.content) {
      const url = URL.createObjectURL(new Blob(['\uFEFF' + message.content], { type: 'text/tab-separated-values;charset=utf-8' }));
      const a = document.createElement('a'); a.href = url; a.download = 'vocabulary.tsv'; a.click();
      window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    }
    status(message.action === 'clear' ? '生词记录已清空。' : message.rows.length ? '显示最近 500 条以内记录；导出包含全部保留记录。' : '暂无记录。开启统计并选用带译文的候选后，点击刷新。');
  });
}
