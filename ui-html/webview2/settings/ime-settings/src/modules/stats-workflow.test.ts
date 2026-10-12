import {afterEach, beforeEach, expect, it, vi} from 'vitest';
import partial from '../partials/stats.html?raw';
import type {ServerMessage} from '../../../../shared/messages';

const configWrites = vi.hoisted(() => vi.fn());
vi.mock('./config-sync', () => ({updateConfig: configWrites}));
vi.mock('./theme', () => ({setSurfaceTheme: vi.fn(), setThemeMode: vi.fn()}));

// An in-memory DOM fixture exercises the real partial's IDs and production renderer/messages.
// It does not claim browser geometry or Windows visual verification.
class Node extends EventTarget {
  children: Node[] = [];
  className = '';
  hidden = false;
  disabled = false;
  value = '';
  title = '';
  colSpan = 1;
  scrollLeft = 0;
  scrollWidth = 795;
  clientWidth = 795;
  offsetParent: object | null = {};
  selectedOptions = [{textContent: '保留最近 90 天'}];
  private text = '';
  private attributes = new Map<string, string>();
  style = {setProperty: vi.fn(), height: '', width: ''};
  classList = {
    contains: (name: string) => this.className.split(/\s+/).includes(name),
    add: (name: string) => {if (!this.classList.contains(name)) this.className += ` ${name}`;},
    toggle: (name: string, force?: boolean) => {
      const next = force ?? !this.classList.contains(name);
      this.className = this.className.split(/\s+/).filter(x => x && x !== name).join(' ');
      if (next) this.classList.add(name);
      return next;
    }
  };
  tag: string;
  constructor(tag: string) {super(); this.tag = tag;}
  get textContent(): string {return this.text + this.children.map(x => x.textContent).join('');}
  set textContent(text: string) {this.text = text; this.children = [];}
  append(...nodes: Node[]): void {
    for (const node of nodes) this.children.push(...(node.tag === '#fragment' ? node.children : [node]));
  }
  replaceChildren(...nodes: Node[]): void {this.children = []; this.text = ''; this.append(...nodes);}
  setAttribute(name: string, value: string): void {this.attributes.set(name, value);}
  getAttribute(name: string): string | null {return this.attributes.get(name) ?? null;}
  click(): void {if (!this.disabled) this.dispatchEvent(new Event('click'));}
}
class Document extends EventTarget {
  nodes = new Map<string, Node>();
  visibilityState = 'visible';
  constructor() {
    super();
    for (const match of partial.matchAll(/<([a-z][\w-]*)\b([^>]*\bid="([^"]+)"[^>]*)>/g)) {
      const node = new Node(match[1]);
      node.hidden = /\bhidden\b/.test(match[2]);
      node.disabled = /\bdisabled\b/.test(match[2]);
      node.className = /\bclass="([^"]*)"/.exec(match[2])?.[1] ?? '';
      this.nodes.set(match[3], node);
    }
    this.nodes.set('stats', new Node('div'));
  }
  getElementById(id: string): Node | null {return this.nodes.get(id) ?? null;}
  createElement(tag: string): Node {return new Node(tag);}
  createDocumentFragment(): Node {return new Node('#fragment');}
}
class Webview extends EventTarget {postMessage = vi.fn();}
class Window extends EventTarget {
  chrome = {webview: new Webview()};
  setTimeout = globalThis.setTimeout;
  clearTimeout = globalThis.clearTimeout;
  setInterval = globalThis.setInterval;
  clearInterval = globalThis.clearInterval;
  confirm = vi.fn(() => false);
}

type StatsResponse = Extract<ServerMessage, {type: 'statsResponse'}>;
type Overview = NonNullable<StatsResponse['overview']>;
let doc: Document, win: Window, stats: typeof import('./stats');
const node = (id: string) => doc.nodes.get(id)!;
const requests = () => win.chrome.webview.postMessage.mock.calls.map(([raw]) => JSON.parse(raw as string));
const lastRequest = () => requests().at(-1)!;
function deliver(request = lastRequest(), overview = snapshot(), extra: Partial<StatsResponse> = {}): void {
  const response: StatsResponse = {type: 'statsResponse', protocolVersion: 1, requestId: request.data.requestId,
    action: request.data.action, ok: true, message: '已加载统计数据', overview, ...extra};
  const event = new Event('message');
  Object.defineProperty(event, 'data', {value: response});
  win.chrome.webview.dispatchEvent(event);
}
function snapshot(days = 80): Overview {
  const today = new Date(2026, 9, 12);
  const daily = Array.from({length: days}, (_, index) => ({dayKey: stats.dateToDayKey(stats.addDays(today, index - days + 1)),
    cjk: index + 1, latin: 2, digit: 3, punct: 4, other: 5, activeMs: 60_000}));
  const categories = {cjk: 0, latin: 0, digit: 0, punct: 0, other: 0};
  for (const row of daily) for (const key of Object.keys(categories) as Array<keyof typeof categories>) categories[key] += row[key];
  return {hasData: days > 0, firstDayKey: daily[0]?.dayKey ?? 0, lastDayKey: daily.at(-1)?.dayKey ?? 0, days,
    totalChars: daily.reduce((sum, row) => sum + stats.totalOf(row), 0), totalActiveMs: days * 60_000,
    todayDayKey: stats.dateToDayKey(today), todayChars: daily.at(-1) ? stats.totalOf(daily.at(-1)!) : 0,
    todayActiveMs: days ? 60_000 : 0, averagePerDay: days ? (days + 1) / 2 + 14 : 0,
    currentStreak: days, longestStreak: days, bestDayKey: daily.at(-1)?.dayKey ?? 0,
    bestDayChars: daily.at(-1) ? stats.totalOf(daily.at(-1)!) : 0, todaySpeed: days ? days + 2 : 0,
    averageSpeed: days ? (days + 1) / 2 + 2 : 0, fastestSpeed: days ? days + 2 : 0,
    fastestDayKey: daily.at(-1)?.dayKey ?? 0, categories,
    todayHourly: Array.from({length: 24}, (_, hour) => days ? hour : 0), daily};
}
function tableBody(): Node {return node('statsDetails').children[0].children[1];}

beforeEach(async () => {
  vi.resetModules(); vi.useFakeTimers(); vi.setSystemTime(new Date(2026, 9, 12, 12)); configWrites.mockClear();
  doc = new Document(); win = new Window(); vi.stubGlobal('document', doc); vi.stubGlobal('window', win);
  stats = await import('./stats'); stats.setupStats();
});
afterEach(() => {vi.useRealTimers(); vi.unstubAllGlobals();});

it('renders every previous dimension from a schema-validated host message and defaults to the latest 30 records', () => {
  deliver();
  expect(node('statsContent').hidden).toBe(false);
  expect(node('statsCards').children).toHaveLength(4);
  expect(node('statsSpeedGrid').children).toHaveLength(3);
  expect(node('statsCalendarGrid').children.length).toBeGreaterThanOrEqual(53 * 7);
  expect(node('statsCalendarMonths').children.length).toBeGreaterThanOrEqual(53);
  expect(node('statsHourly').children).toHaveLength(24);
  expect(node('statsCategories').children).toHaveLength(5);
  expect(tableBody().children).toHaveLength(30);
  expect(tableBody().children.every(row => row.children.length === 9)).toBe(true);
  expect(tableBody().children[0].children[0].textContent).toBe('2026-10-12');
  expect(node('statsDetailPage').textContent).toContain('共 80 天');
  expect(requests()).toHaveLength(1);
  expect(configWrites).not.toHaveBeenCalled();
});

it('keeps the full statistics framework for an actual empty snapshot without inventing day records', () => {
  stats.applyStatisticsEnabled(true);
  expect(node('statsNoDataEmpty').hidden).toBe(true);
  deliver(lastRequest(), snapshot(0));
  expect(node('statsNoDataEmpty').hidden).toBe(false);
  expect(node('statsContent').hidden).toBe(false);
  expect(node('statsCards').children).toHaveLength(4);
  expect(node('statsHourly').children).toHaveLength(24);
  expect(tableBody().children).toHaveLength(1);
  expect(tableBody().children[0].children[0].colSpan).toBe(9);
  expect(tableBody().textContent).toContain('暂无按日记录');
  expect(node('statsDetailNext').disabled).toBe(true);
  expect(configWrites).not.toHaveBeenCalled();
});

it('exposes all retained daily history with inclusive date filtering, validates reversed dates and preserves summary charts', () => {
  deliver(); const cards = node('statsCards').textContent;
  node('statsDetailNext').click(); expect(node('statsDetailPage').textContent).toContain('第 2/3 页');
  expect(tableBody().children[0].children[0].textContent).toBe('2026-09-12');
  node('statsDetailNext').click(); expect(tableBody().children).toHaveLength(20);
  expect(node('statsDetailNext').disabled).toBe(true);
  node('statsDetailPrevious').click(); expect(node('statsDetailPage').textContent).toContain('第 2/3 页');
  node('statsDetailFrom').value = '2026-10-01'; node('statsDetailTo').value = '2026-10-05';
  node('statsDetailTo').dispatchEvent(new Event('change'));
  expect(tableBody().children).toHaveLength(5);
  expect(tableBody().children[0].children[0].textContent).toBe('2026-10-05');
  expect(tableBody().children.at(-1)!.children[0].textContent).toBe('2026-10-01');
  node('statsDetailFrom').value = '2026-10-10'; node('statsDetailFrom').dispatchEvent(new Event('change'));
  expect(node('statsDetailRangeStatus').textContent).toContain('不能晚于');
  expect(tableBody().children).toHaveLength(5);
  node('statsDetailReset').click(); expect(tableBody().children).toHaveLength(30);
  expect(node('statsCards').textContent).toBe(cards);
  expect(requests()).toHaveLength(1); expect(configWrites).not.toHaveBeenCalled();
});

it('retains historical metrics when recording is disabled and preserves the configured retention value', () => {
  deliver(); stats.applyStatisticsEnabled(false); stats.applyStatisticsRetention('90d');
  expect(node('statsDisabledText').textContent).toContain('历史数据仍保留');
  expect(node('statsContent').hidden).toBe(false); expect(node('statsRetention').value).toBe('90d');
  expect(configWrites).not.toHaveBeenCalled();
  node('statsRetention').dispatchEvent(new Event('change'));
  expect(configWrites).toHaveBeenCalledWith('statistics.retention', '90d');
});

it('keeps a valid dashboard on read failure and ignores a superseded slow overview response', () => {
  const old = lastRequest(); vi.advanceTimersByTime(20_000); const fresh = lastRequest();
  expect(fresh.data.requestId).not.toBe(old.data.requestId);
  deliver(fresh); const cards = node('statsCards').textContent;
  deliver(old, snapshot(0)); expect(node('statsCards').textContent).toBe(cards);
  vi.advanceTimersByTime(5000); deliver(lastRequest(), snapshot(0), {ok: false, message: '读取失败，请重试'});
  expect(node('statsStatus').hidden).toBe(false); expect(node('statsStatus').textContent).toContain('读取失败');
  expect(node('statsCards').textContent).toBe(cards);
  vi.advanceTimersByTime(5000); deliver(lastRequest());
  expect(node('statsStatus').hidden).toBe(true);
});

it('does not request data in the hidden page and requires confirmation before clearing statistics', () => {
  deliver(); node('stats').offsetParent = null; vi.advanceTimersByTime(15_000);
  expect(requests()).toHaveLength(1);
  node('stats').offsetParent = {}; win.dispatchEvent(new Event('focus'));
  expect(requests()).toHaveLength(2); deliver();
  node('statsClearButton').click(); expect(requests()).toHaveLength(2);
  win.confirm.mockReturnValue(true); node('statsClearButton').click();
  expect(lastRequest().data.action).toBe('clearAll');
  deliver(lastRequest(), snapshot(0), {removedDays: 80, message: '已清空 80 天数据'});
  expect(node('statsContent').hidden).toBe(false); expect(node('statsClearStatus').textContent).toContain('80');
  expect(tableBody().textContent).toContain('暂无按日记录');
});
