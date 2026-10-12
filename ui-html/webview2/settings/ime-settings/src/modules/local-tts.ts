import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

function formatModelBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '大小未知';
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(2)} GB` : `${(bytes / 1e6).toFixed(1)} MB`;
}

export function setupLocalTts(): void {
  const container = document.getElementById('localTtsModels');
  const status = document.getElementById('localTtsStatus');
  if (!container || !status || container.dataset.ready) return;
  container.dataset.ready = '1';
  let counter = 0;
  let busy = false;
  let timer = 0;
  let activeModel = "";
  let renderedModels = '';
  const descriptionOpen = new Map<string,boolean>();
  const send = (action: 'status' | 'download' | 'cancel' | 'start' | 'stop', modelId?: string) => {
    const requestId = `local-tts-${++counter}`;
    window.chrome?.webview?.postMessage(serializeHostMessage({type:'localTtsRequest', data:{requestId, action, ...(modelId ? {modelId} : {})}}));
  };
  const add = (parent: HTMLElement, tag: string, text: string, className = '') => {
    const node = document.createElement(tag); node.textContent = text; node.className = className;
    parent.appendChild(node); return node;
  };
  const actionButton = (parent: HTMLElement, label: string, action: 'download' | 'cancel' | 'start' | 'stop', id: string, disabled = false) => {
    const button = add(parent, 'button', label, 'credential-test-button') as HTMLButtonElement;
    button.type = 'button'; button.disabled = disabled;
    button.addEventListener('click', () => { button.disabled = true; renderedModels = ''; send(action, id); });
  };
  onHostMessage('localTtsResponse', message => {
    if (message.type !== 'localTtsResponse') return;
    const service = message.service;
    const readyModel = service.state === 'running' ? String(service.model ?? '') : '';
    if (readyModel && readyModel !== activeModel) window.chrome?.webview?.postMessage(serializeHostMessage({type:'configRequest'}));
    activeModel = readyModel;
    if (!message.ok || message.message) status.textContent = message.message;
    else status.textContent = typeof service.message === 'string' ? service.message : '选择模型，下载完成后点击“启动并使用”。';
    // Status polling must not replace focused buttons or interrupt a click.
    // Actions invalidate this snapshot so an unchanged error response can
    // restore the temporarily disabled button too.
    const modelSnapshot = JSON.stringify([message.models, service.model, service.state]);
    if (modelSnapshot === renderedModels) return;
    renderedModels = modelSnapshot;
    container.replaceChildren(); busy = false;
    for (const row of message.models) {
      const id = String(row.id ?? '');
      const state = (row.status ?? {}) as Record<string, unknown>;
      const stage = String(state.state ?? 'absent');
      const downloading = ['downloading', 'verifying', 'extracting'].includes(stage);
      busy ||= downloading || ['loading', 'switching'].includes(String(service.state));
      const card = add(container, 'article', '', 'local-asr-card');
      add(card, 'h4', String(row.name));
      const online = row.engine === 'edge';
      const awaiting = row.availability === 'awaiting_weights';
      add(card, 'p', awaiting ? `${row.format} · 等待官方权重，下载大小尚不可核实` : `${row.format} · ${online ? '无需下载模型权重' : `下载 ${formatModelBytes(Number(row.downloadBytes))}`} · 建议磁盘预留 ${formatModelBytes(Number(row.diskBytes))}`, 'local-asr-size');
      const description = add(card, 'details', '', 'model-description');
      (description as HTMLDetailsElement).open=descriptionOpen.get(id)??false;
      description.addEventListener('toggle',()=>descriptionOpen.set(id,(description as HTMLDetailsElement).open));
      add(description, 'summary', '模型特点、语言与运行要求');
      add(description, 'p', String(row.features));
      add(description, 'p', String(row.hardware), 'voice-description');
      if(row.dialectNote) add(description,'p',String(row.dialectNote),'voice-description');
      if(row.parameterNote) add(description,'p',String(row.parameterNote),'voice-description');
      const links = add(card, 'div', '', 'local-asr-actions');
      const link = add(links, 'a', '官方介绍 / 原版下载') as HTMLAnchorElement;
      link.href = String(row.official); link.addEventListener('click', event => {
        event.preventDefault(); window.chrome?.webview?.postMessage(serializeHostMessage({type:'openExternalUrl',data:link.href}));
      });
      if (awaiting) { add(card, 'p', '官方完整权重尚不可获取，本版暂不能下载或运行此模型。', 'voice-description'); continue; }
      const external = row.engine === 'external';
      const downloadLink = add(links, 'a', '官方下载网址') as HTMLAnchorElement;
      downloadLink.href = String(row.downloadUrl); downloadLink.addEventListener('click', event => {event.preventDefault();window.chrome?.webview?.postMessage(serializeHostMessage({type:'openExternalUrl',data:downloadLink.href}));});
      if (external) { add(card, 'p', '外部部署：按官方指南下载运行后，在上方填写本机兼容服务地址。此处显示仓库文件合计，额外依赖另计。', 'voice-description'); continue; }
      if (!online) actionButton(links, stage === 'ready' ? '重新校验下载' : stage === 'paused' || stage === 'error' ? '继续下载' : '下载运行包', 'download', id, downloading);
      if (downloading) actionButton(links, '暂停下载', 'cancel', id);
      const current = service.model === id && ['running', 'loading', 'switching'].includes(String(service.state));
      const label = current ? service.state === 'running' ? '正在使用 · 点击停止' : '正在启动 · 点击停止' : online ? '启用在线服务（文本发送到微软）' : '启动并使用';
      actionButton(links, label, current ? 'stop' : 'start', id, !current && !online && stage !== 'ready');
      if (downloading || stage === 'paused') {
        const progress = add(card, 'progress', '') as HTMLProgressElement;
        progress.max = Number(row.downloadBytes); progress.value = Number(state.downloaded ?? 0);
        add(card, 'p', `${formatModelBytes(Number(state.downloaded ?? 0))} / ${formatModelBytes(Number(row.downloadBytes))} · ${Math.min(100, progress.value / progress.max * 100).toFixed(1)}%`, 'local-asr-size');
      }
      if (typeof state.message === 'string' && state.message) add(card, 'p', state.message.replace('本地识别', '本地发音'), 'voice-description');
    }
  });
  document.getElementById('localTtsRefresh')?.addEventListener('click', () => send('status'));
  document.getElementById('localTtsStop')?.addEventListener('click', () => send('stop'));
  const panel = document.getElementById('localTtsPanel') as HTMLDetailsElement | null;
  const refreshPanel = () => {
    window.clearInterval(timer);
    if (panel?.open) {
      send('status');
      timer = window.setInterval(() => { if (!document.hidden || busy) send('status'); }, 1500);
    }
  };
  panel?.addEventListener('toggle', refreshPanel);
  if (panel?.open) refreshPanel();
  window.addEventListener('beforeunload', () => window.clearInterval(timer));
}
