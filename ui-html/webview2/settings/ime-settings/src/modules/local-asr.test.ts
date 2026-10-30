import { afterEach, beforeEach, expect, it, vi } from 'vitest';
const handlers = vi.hoisted(() => new Map<string, (m: any) => void>());
vi.mock('../utils/host-messages', () => ({ onHostMessage: (type: string, fn: any) => handlers.set(type, fn) }));
import { formatModelBytes, setupLocalAsr } from './local-asr';
class Element {
  dataset: Record<string, string> = {};
  textContent = ''; children: Element[] = []; events = new Map<string, any>();
  open = false; disabled = false; href = ''; value = 0; max = 0;
  tag: string;
  constructor(tag = '') { this.tag = tag; }
  appendChild(child: Element) { this.children.push(child); }
  replaceChildren() { this.children = []; }
  addEventListener(event: string, fn: any) { this.events.set(event, fn); }
}
let elements: Map<string, Element>, sent: any[];
beforeEach(() => {
  vi.useFakeTimers(); handlers.clear(); sent = [];
  elements = new Map(['localAsrModels','localAsrStatus','localAsrRefresh','localAsrStop','localAsrPanel'].map(id => [id,new Element()]));
  vi.stubGlobal('document', {getElementById: (id: string) => elements.get(id), createElement: (tag: string) => new Element(tag), hidden: false});
  vi.stubGlobal('window', {chrome: {webview: {postMessage: (m: any) => sent.push(typeof m === 'string' ? JSON.parse(m) : m)}}, clearInterval, setInterval, addEventListener: vi.fn()});
});
afterEach(() => {vi.useRealTimers();vi.unstubAllGlobals();});
it('shows locked download bytes using decimal MB/GB, not RAM sizes', () => {
  expect(formatModelBytes(163002883)).toBe('163.0 MB');
  expect(formatModelBytes(4703055333)).toBe('4.70 GB');
  expect(formatModelBytes(NaN)).toBe('大小未知');
});
it('opens with a schema-valid status request and stops polling on collapse', () => {
  setupLocalAsr(); const panel=elements.get('localAsrPanel')!;
  panel.open=true;panel.events.get('toggle')();
  expect(sent.at(-1).data).toEqual({requestId:'local-asr-1',action:'status'});
  vi.advanceTimersByTime(1500); expect(sent).toHaveLength(2);
  panel.open=false;panel.events.get('toggle')();vi.advanceTimersByTime(3000);expect(sent).toHaveLength(2);
});
it('renders downloaded/total progress and only opens explicit official links', () => {
  setupLocalAsr();handlers.get('localAsrResponse')!({type:'localAsrResponse',ok:true,message:'',service:{},models:[{
    id:'sensevoice-small',name:'SenseVoice',format:'ONNX INT8',downloadBytes:163002883,diskBytes:489008649,originalBytes:938000000,
    features:'离线识别',hardware:'内存建议',official:'https://huggingface.co/FunAudioLLM/SenseVoiceSmall',
    status:{state:'downloading',downloaded:81501441}
  }]});
  const card=elements.get('localAsrModels')!.children[0];
  expect(card.children.some(x=>x.textContent.includes('81.5 MB / 163.0 MB · 50.0%'))).toBe(true);
  const progress=card.children.find(x=>x.tag==='progress')!;expect(progress.max).toBe(163002883);
  const actions=card.children.find(x=>x.tag==='div')!;
  expect(actions.children.find(x=>x.textContent==='启动并使用')?.disabled).toBe(true);
  expect(sent).toHaveLength(0);
  actions.children[0].events.get('click')({preventDefault:vi.fn()});
  expect(sent.at(-1)).toMatchObject({type:'openExternalUrl',data:'https://huggingface.co/FunAudioLLM/SenseVoiceSmall'});
});
