import { afterEach, beforeEach, expect, it, vi } from 'vitest';
const handlers = vi.hoisted(() => new Map<string, (m: any) => void>());
vi.mock('../utils/host-messages', () => ({ onHostMessage: (type: string, fn: any) => handlers.set(type, fn) }));
import { setupLocalTts } from './local-tts';
class Element {
  dataset: Record<string, string> = {}; textContent = ''; children: Element[] = [];
  events = new Map<string, any>(); open = false; disabled = false; href = ''; value = 0; max = 0;
  tag: string;
  constructor(tag = '') { this.tag = tag; }
  appendChild(child: Element) { this.children.push(child); }
  replaceChildren() { this.children = []; }
  addEventListener(event: string, fn: any) { this.events.set(event, fn); }
}
let elements: Map<string, Element>, sent: any[];
const models = [
  {id:'kokoro', name:'Kokoro',engine:'kokoro',status:{state:'ready'}},
  {id:'moss-nano', name:'MOSS',engine:'moss',status:{state:'ready'}},
  {id:'qwen-1.7b',name:'Qwen',engine:'qwen',status:{}},
  {id:'edge-tts',name:'Edge',engine:'edge',status:{}}
].map(m=>({...m,format:'runtime',downloadBytes:1000,diskBytes:2000,official:'https://github.com/test/test',downloadUrl:'https://github.com/test/test',features:'features',hardware:'hardware'}));
beforeEach(() => {
  vi.useFakeTimers(); handlers.clear(); sent=[];
  elements = new Map(['localTtsModels','localTtsStatus','localTtsRefresh','localTtsStop','localTtsPanel'].map(id=>[id,new Element()]));
  vi.stubGlobal('document',{getElementById:(id:string)=>elements.get(id),createElement:(tag:string)=>new Element(tag),hidden:false});
  vi.stubGlobal('window',{chrome:{webview:{postMessage:(m:any)=>sent.push(JSON.parse(m))}},clearInterval,setInterval,addEventListener:vi.fn()});
});
afterEach(()=>{vi.useRealTimers();vi.unstubAllGlobals();});
function response(state:string) {
  handlers.get('localTtsResponse')!({type:'localTtsResponse',ok:true,message:'',service:{model:'kokoro',state},models});
}
function actions(index:number) { return elements.get('localTtsModels')!.children[index].children.find(c=>c.tag==='div')!.children.filter(c=>c.tag==='button'); }
it('stops the current model and lets another downloaded model replace it in one click',()=>{
  setupLocalTts(); response('running');
  const stop=actions(0).find(b=>b.textContent.includes('正在使用'))!;
  expect(stop.disabled).toBe(false); stop.events.get('click')();
  expect(sent.at(-1)).toMatchObject({type:'localTtsRequest',data:{action:'stop',modelId:'kokoro'}});
  const start=actions(1).find(b=>b.textContent==='启动并使用')!;
  expect(start.disabled).toBe(false); start.events.get('click')();
  expect(sent.at(-1)).toMatchObject({data:{action:'start',modelId:'moss-nano'}});
  expect(actions(2).find(b=>b.textContent==='启动并使用')!.disabled).toBe(true);
});
it('allows cancellation while loading and clearly opts into Edge online without downloading weights',()=>{
  setupLocalTts(); response('loading');
  const stop=actions(0).find(b=>b.textContent.includes('正在启动'))!;
  expect(stop.disabled).toBe(false); stop.events.get('click')();
  expect(sent.at(-1).data.action).toBe('stop');
  const online=actions(3);
  expect(online).toHaveLength(1);
  expect(online[0].textContent).toContain('文本发送到微软');
  expect(online[0].disabled).toBe(false);
  online[0].events.get('click')(); expect(sent.at(-1).data).toMatchObject({action:'start',modelId:'edge-tts'});
});
it('polls an initially open panel and refreshes configuration only after the model is ready',()=>{
  elements.get('localTtsPanel')!.open=true; setupLocalTts();
  expect(sent.at(-1).data.action).toBe('status');
  response('switching'); expect(sent.filter(m=>m.type==='configRequest')).toHaveLength(0);
  response('running'); response('running'); expect(sent.filter(m=>m.type==='configRequest')).toHaveLength(1);
  vi.advanceTimersByTime(1500); expect(sent.at(-1).data.action).toBe('status');
});
it('preserves buttons on unchanged polls and restores an action after an unchanged response',()=>{
  setupLocalTts(); response('running');
  const stop=actions(0).find(b=>b.textContent.includes('正在使用'))!;
  response('running');
  expect(actions(0).find(b=>b.textContent.includes('正在使用'))).toBe(stop);
  stop.events.get('click')(); expect(stop.disabled).toBe(true);
  response('running');
  expect(actions(0).find(b=>b.textContent.includes('正在使用'))!.disabled).toBe(false);
  response('stopped');
  expect(actions(0).find(b=>b.textContent==='启动并使用')!.disabled).toBe(false);
});
