import { beforeEach, afterEach, expect, it, vi } from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(message:any)=>void>());
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string, fn:(message:any)=>void)=>handlers.set(type,fn)}));
class Element {
  dataset:Record<string,string>={};children:Element[]=[];value='';textContent='';disabled=false;checked=false;href='';
  onchange?:()=>void;onclick?:()=>void;oninput?:()=>void;
  querySelector(selector:string){return fields.get(selector);}
  append(...nodes:Element[]){this.children.push(...nodes);}
  replaceChildren(){this.children=[];}
  setAttribute(){}
}
let root:Element, fields:Map<string,Element>, sent:any[];
const book=(id:string, enabled=true)=>({id,name:id,category:'学习',language:'中英双向',headwords:10,entries:20,bytes:1024,note:'历史标签',source:'https://example.org',license:'MIT',version:'1',sha256:'a',enabled});
const respond=(request:any,books=[book('study-cet4'),book('study-cet6')],ok=true)=>handlers.get('dictionaryLibraryResponse')!({requestId:request.data.requestId,ok,books,message:ok?'':'读取失败'});
beforeEach(async()=>{
  vi.resetModules();vi.useFakeTimers();handlers.clear();sent=[];root=new Element();
  fields=new Map(['[data-status]','[data-books]','[data-search]','[data-category]','[data-refresh]'].map(k=>[k,new Element()]));
  fields.get('[data-category]')!.value='all';
  vi.stubGlobal('document',{getElementById:()=>root,createElement:()=>new Element(),createTextNode:(text:string)=>Object.assign(new Element(),{textContent:text})});
  vi.stubGlobal('window',{chrome:{webview:{postMessage:(raw:string)=>sent.push(JSON.parse(raw))}}});
  (await import('./translation-library')).setupTranslationLibrary();
});
afterEach(()=>{vi.useRealTimers();vi.unstubAllGlobals();});
it('renders all books, filters, and exports the selected id',()=>{
  respond(sent[0]);
  const list=fields.get('[data-books]')!;
  expect(list.children).toHaveLength(2);
  fields.get('[data-search]')!.value='cet6';fields.get('[data-search]')!.oninput!();
  expect(list.children).toHaveLength(1);
  list.children[0].children[3].children[1].onclick!();
  expect(sent.at(-1).data).toMatchObject({action:'export',id:'study-cet6'});
});
it('persists independent switches, waits for the host and recovers from missing acknowledgement',()=>{
  respond(sent[0],[book('study-cet4'),book('study-cet6',false)]);
  let toggle=fields.get('[data-books]')!.children[0].children[0].children[0];
  toggle.checked=false;toggle.onchange!();
  expect(sent.at(-1).data).toEqual({path:'general.disabled_translation_dictionaries',value:'study-cet4,study-cet6'});
  expect(fields.get('[data-books]')!.children[0].children[0].children[0].disabled).toBe(true);
  handlers.get('configSnapshot')!({});respond(sent.at(-1),[book('study-cet4',false),book('study-cet6',false)]);
  toggle=fields.get('[data-books]')!.children[0].children[0].children[0];
  expect(toggle.checked).toBe(false);expect(toggle.disabled).toBe(false);
  toggle.checked=true;toggle.onchange!();vi.advanceTimersByTime(5000);
  expect(fields.get('[data-status]')!.textContent).toContain('未收到保存确认');
  expect(fields.get('[data-books]')!.children[0].children[0].children[0].disabled).toBe(false);
});
it('does not let an export response cancel a pending switch and handles read failure',()=>{
  respond(sent[0]);
  const first=fields.get('[data-books]')!.children[0];
  first.children[3].children[1].onclick!();const exportRequest=sent.at(-1);
  first.children[0].children[0].checked=false;first.children[0].children[0].onchange!();
  respond(exportRequest);
  expect(fields.get('[data-books]')!.children[0].children[0].children[0].disabled).toBe(true);
  handlers.get('configSnapshot')!({});respond(sent.at(-1),[],false);
  expect(fields.get('[data-status]')!.textContent).toBe('读取失败');
  expect(fields.get('[data-books]')!.children[0].children[0].children[0].disabled).toBe(false);
});
