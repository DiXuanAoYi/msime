import {afterEach, beforeEach, expect, it, vi} from 'vitest';
vi.mock('../utils/host-messages', () => ({onHostMessage: vi.fn()}));
vi.mock('./config-sync', () => ({updateConfig: vi.fn()}));
import {onHostMessage} from '../utils/host-messages';
import {domainPackSourceLinks, domainVersionLabel, setupDomainSettings} from './domain-settings';

class Node extends EventTarget {
  value = ''; textContent = ''; type = ''; tag = ''; download = '';
  children: Node[] = [];
  append(...nodes: Node[]): void { this.children.push(...nodes); }
  prepend(...nodes: Node[]): void { this.children.unshift(...nodes); }
  replaceChildren(...nodes: Node[]): void { this.children = nodes; }
  querySelectorAll(): Node[] { return []; }
  click = vi.fn();
}
let nodes: Map<string, Node>, created: Node[];
let receive: (value: any) => void;
beforeEach(() => {
  vi.clearAllMocks();
  created = [];
  nodes = new Map(['domainCatalog', 'domainStatus', 'domainRefresh', 'domainApplication',
    'domainApplicationMode', 'domainApplicationPacks'].map(id => [id, new Node()]));
  nodes.get('domainApplicationMode')!.value = 'inherit';
  vi.stubGlobal('window', {chrome: {webview: {postMessage: vi.fn()}}});
  vi.stubGlobal('document', {
    getElementById: (id: string) => nodes.get(id) ?? null,
    createElement: (tag: string) => { const node = Object.assign(new Node(), {tag}); created.push(node); return node; },
    createTextNode: (textContent: string) => Object.assign(new Node(), {textContent}),
  });
  vi.mocked(onHostMessage).mockImplementation((_type, callback) => { receive = callback; return () => {}; });
  setupDomainSettings();
  receive({requestId: 'domains-domainCatalog-initial', ok: true, message: '', books: [],
    enabled_domains: [], applications: []});
});
afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers(); });
const pack = {id: 'idioms', name: '成语', entries: 1, bytes: 24, license: 'MIT', source: 'https://example.org/dict',
  version: 'v1', sha256: 'b'.repeat(64), exportable: true};

const commit = 'dd9c471766e117c1f8c557042560776179fe9bfe';
it('links descriptive Qingjian source metadata to the pinned actual domain file', () => {
  for (const [id, filename] of [['animals', 'animals.tsv'], ['historical', 'historical_figures.tsv'],
    ['it', 'it_computing.tsv'], ['poetry', 'poetry_lines.tsv']]) {
    const links = domainPackSourceLinks({id, source: 'qingjian/THUOCL + Unihan', version: commit});
    expect(links[0].url).toBe(`https://github.com/qingjian-team/qingjian/blob/${commit}/assets/lexicon/03_domains/${filename}`);
    expect(links.map(link => new URL(link.url).protocol)).toEqual(['https:', 'https:', 'https:']);
    expect(links.map(link => new URL(link.url).hostname)).toEqual(['github.com', 'github.com', 'www.unicode.org']);
  }
});

it('accepts only absolute HTTP(S) sources and never turns a description into a settings-relative link', () => {
  for (const source of ['https://example.org/dict', 'http://example.org/dict']) {
    expect(domainPackSourceLinks({id: 'custom', source, version: 'v1'})[0].url).toBe(source);
  }
  for (const source of ['relative.tsv', '/dict.tsv', '//example.org/dict', 'javascript:alert(1)',
    'data:text/html,test', 'file:///C:/dict.tsv', 'https://user:secret@example.org/dict']) {
    expect(domainPackSourceLinks({id: 'custom', source, version: 'v1'})).toEqual([]);
  }
  expect(domainPackSourceLinks({id: 'unknown', source: 'qingjian/THUOCL + Unihan', version: '../main'})[0].url)
    .toBe('https://github.com/qingjian-team/qingjian');
});

it('shows a compact commit without mutating the complete version or ordinary version names', () => {
  expect(domainVersionLabel(commit)).toBe('dd9c4717');
  expect(commit).toHaveLength(40);
  expect(domainVersionLabel('v2026.10')).toBe('v2026.10');
  expect(domainVersionLabel('')).toBe('未知');
});

it('shows optional catalog warnings from successful original library actions and removes stale export controls', () => {
  receive({requestId: 'domains-domainCatalog-1', ok: true, message: '', books: [pack]});
  expect(nodes.get('domainCatalog')!.children).toHaveLength(1);
  receive({requestId: 'library-putClip-2', ok: true, message: '常用语已保存', books: [],
    domainWarning: '领域词库清单无法加载；其他资料已保存', book: {clips: [{text: '保留内容'}]}});
  expect(nodes.get('domainStatus')!.textContent).toBe('领域词库清单无法加载；其他资料已保存');
  expect(nodes.get('domainCatalog')!.children).toHaveLength(0);
  receive({requestId: 'domains-domainCatalog-3', ok: true, message: '', books: [pack]});
  expect(nodes.get('domainStatus')!.textContent).toBe('');
  expect(nodes.get('domainCatalog')!.children).toHaveLength(1);
});

it('clears stale packs on catalog failure without discarding valid catalog after another domain action fails', () => {
  receive({requestId: 'domains-domainCatalog-1', ok: true, message: '', books: [pack]});
  receive({requestId: 'domains-exportDomain-2', ok: false, message: '导出文件不存在'});
  expect(nodes.get('domainCatalog')!.children).toHaveLength(1);
  receive({requestId: 'domains-domainCatalog-3', ok: false, message: '领域词库清单无效'});
  expect(nodes.get('domainStatus')!.textContent).toBe('领域词库清单无效');
  expect(nodes.get('domainCatalog')!.children).toHaveLength(0);
});

it('uses the backend pack-specific export name and rejects an unsafe filename', () => {
  vi.useFakeTimers();
  const revoke = vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:test-owned');
  receive({requestId: 'domains-exportDomain-1', ok: true, message: '', exported: '测试词条',
    filename: 'msime-domain-idioms.tsv', books: []});
  const first = created.filter(node => node.tag === 'a').at(-1)!;
  expect(first.download).toBe('msime-domain-idioms.tsv');
  expect(first.click).toHaveBeenCalledOnce();
  receive({requestId: 'domains-exportDomain-2', ok: true, message: '', exported: '测试词条', filename: '../other.tsv'});
  expect(created.filter(node => node.tag === 'a').at(-1)!.download).toBe('知语-领域输入词库.tsv');
  vi.advanceTimersByTime(1000);
  expect(revoke).toHaveBeenCalledTimes(2);
});
