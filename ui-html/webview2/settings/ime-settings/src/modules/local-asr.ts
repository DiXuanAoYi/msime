import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

export function formatModelBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '大小未知';
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(2)} GB` : `${(bytes / 1e6).toFixed(1)} MB`;
}

export function setupLocalAsr(): void {
  const container = document.getElementById('localAsrModels');
  const status = document.getElementById('localAsrStatus');
  if (!container || !status || container.dataset.ready) return;
  container.dataset.ready = '1';
  let counter = 0;
  let busy = false;
  let timer = 0;
  const send = (action: 'status' | 'download' | 'cancel' | 'start' | 'stop', modelId?: string) => {
    window.chrome?.webview?.postMessage(serializeHostMessage({type:'localAsrRequest', data:{requestId:`local-asr-${++counter}`, action, ...(modelId ? {modelId} : {})}}));
  };
  const add = (parent: HTMLElement, tag: string, text: string, className = '') => {
    const node = document.createElement(tag); node.textContent = text; node.className = className;
    parent.appendChild(node); return node;
  };
  const actionButton = (parent: HTMLElement, label: string, action: 'download' | 'cancel' | 'start', id: string, disabled = false) => {
    const button = add(parent, 'button', label, 'credential-test-button') as HTMLButtonElement;
    button.type = 'button'; button.disabled = disabled;
    button.addEventListener('click', () => { button.disabled = true; send(action, id); });
  };
  onHostMessage('localAsrResponse', message => {
    if (message.type !== 'localAsrResponse') return;
    const service = message.service;
    if (!message.ok || message.message) status.textContent = message.message;
    else status.textContent = typeof service.message === 'string' ? service.message : '选择模型，下载完成后点击“启动并使用”。';
    container.replaceChildren(); busy = false;
    for (const row of message.models) {
      const id = String(row.id ?? '');
      const state = (row.status ?? {}) as Record<string, unknown>;
      const stage = String(state.state ?? 'absent');
      const downloading = ['downloading', 'verifying', 'extracting'].includes(stage);
      busy ||= downloading || service.state === 'loading';
      const card = add(container, 'article', '', 'local-asr-card');
      add(card, 'h4', String(row.name));
      add(card, 'p', `${row.format} · 下载 ${formatModelBytes(Number(row.downloadBytes))} · 建议磁盘预留 ${formatModelBytes(Number(row.diskBytes))}`, 'local-asr-size');
      add(card, 'p', String(row.features));
      add(card, 'p', String(row.hardware), 'voice-description');
      if (row.format !== '原版') add(card, 'p', `官方原版权重约 ${formatModelBytes(Number(row.originalBytes))}；下方下载的是 sherpa-onnx 转换运行包，量化可能影响精度。`, 'voice-description');
      const links = add(card, 'div', '', 'local-asr-actions');
      const link = add(links, 'a', '官方介绍 / 原版下载') as HTMLAnchorElement;
      link.href = String(row.official); link.addEventListener('click', event => {
        event.preventDefault(); window.chrome?.webview?.postMessage(serializeHostMessage({type:'openExternalUrl',data:link.href}));
      });
      actionButton(links, stage === 'ready' ? '重新校验下载' : stage === 'paused' || stage === 'error' ? '继续下载' : '下载运行包', 'download', id, downloading);
      if (downloading) actionButton(links, '暂停下载', 'cancel', id);
      actionButton(links, service.model === id && service.state === 'running' ? '正在使用' : '启动并使用', 'start', id, stage !== 'ready' || ['running','loading'].includes(String(service.state)));
      if (downloading || stage === 'paused') {
        const progress = add(card, 'progress', '') as HTMLProgressElement;
        progress.max = Number(row.downloadBytes); progress.value = Number(state.downloaded ?? 0);
        add(card, 'p', `${formatModelBytes(Number(state.downloaded ?? 0))} / ${formatModelBytes(Number(row.downloadBytes))} · ${Math.min(100, progress.value / progress.max * 100).toFixed(1)}%`, 'local-asr-size');
      }
      if (typeof state.message === 'string' && state.message) add(card, 'p', state.message, 'voice-description');
    }
  });
  document.getElementById('localAsrRefresh')?.addEventListener('click', () => send('status'));
  document.getElementById('localAsrStop')?.addEventListener('click', () => send('stop'));
  const panel = document.getElementById('localAsrPanel') as HTMLDetailsElement | null;
  panel?.addEventListener('toggle', () => {
    window.clearInterval(timer);
    if (panel.open) {
      send('status');
      timer = window.setInterval(() => { if (!document.hidden || busy) send('status'); }, 1500);
    }
  });
  window.addEventListener('beforeunload', () => window.clearInterval(timer));
}
