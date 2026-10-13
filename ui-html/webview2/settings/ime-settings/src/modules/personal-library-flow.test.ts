import {readFileSync} from 'node:fs';
import {afterEach,beforeEach,expect,it,vi} from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(value:any)=>void>());
const confirm=vi.hoisted(()=>vi.fn());
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string,cb:(value:any)=>void)=>handlers.set(type,cb)}));
vi.mock('../utils/confirm-dialog',()=>({confirmDialog:confirm}));
vi.mock('./config-sync',()=>({updateConfig:vi.fn()}));
import {setupPersonalLibrary} from './personal-library';
class Element extends EventTarget {
  value='';textContent='';hidden=false;disabled=false;checked=false;open=false;type='';id='';className='';dataset:Record<string,string>={};
  children:Element[]=[];parentElement:Element|null=null;attributes=new Map<string,string>();
  onclick:((e:Event)=>void)|null=null;onchange:((e:Event)=>void)|null=null;ondblclick:((e:Event)=>void)|null=null;
  setAttribute(k:string,v:string){this.attributes.set(k,v);}focus(){}scrollIntoView(){}
  append(...nodes:Element[]){for(const node of nodes){node.parentElement=this;this.children.push(node);}}
  replaceChildren(...nodes:Element[]){this.children=[];this.append(...nodes);}
  querySelectorAll(selector:string):Element[]{
    if(selector==='[data-library-view]')return this.children.filter(x=>x.dataset.libraryView);
    if(selector==='[data-review]')return this.children.filter(x=>x.dataset.review);
    return this.children.filter(x=>selector==='button'||x.className==='clipboard-item');
  }
  override dispatchEvent(e:Event){if(e.type==='change')this.onchange?.(e);return super.dispatchEvent(e);}
  click(){const e=new Event('click');this.onclick?.(e);this.dispatchEvent(e);}
}
let nodes:Map<string,Element>,post:ReturnType<typeof vi.fn>;
beforeEach(()=>{
  handlers.clear();confirm.mockResolvedValue(false);post=vi.fn();
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  nodes=new Map(Array.from(html.matchAll(/id="([^"]+)"/g),m=>[m[1],new Element()]));
  const root=nodes.get('personalLibraryPanel')!;
  for(const m of html.matchAll(/data-library-view="([^"]+)"/g)){const tab=new Element();tab.dataset.libraryView=m[1];root.append(tab);}
  nodes.get('libraryView')!.value='history';nodes.get('libraryCaptureEnabled')!.parentElement=new Element();
  vi.stubGlobal('Option',class extends Element{constructor(text:string,value:string){super();this.textContent=text;this.value=value;}});
  vi.stubGlobal('document',{getElementById:(id:string)=>nodes.get(id),createElement:()=>new Element()});
  vi.stubGlobal('window',{chrome:{webview:{postMessage:post}}});
  setupPersonalLibrary();
  reply(['第一行\n完整多行内容','第二条记录']);post.mockClear();
});
afterEach(()=>vi.unstubAllGlobals());
function reply(history:string[],ok=true){handlers.get('personalLibraryResponse')!({requestId:'library-status-fixture',ok,message:ok?'已读取':'保存失败',history,
  book:{groups:[{id:'phrases',name:'常用语'},{id:'quotes',name:'语录'}],clips:[{id:'saved',name:'旧常用语',text:'保留',group:'phrases'}],rules:[],favorites:[],applications:[],appLearning:[]}});}
function actions(){return post.mock.calls.map(x=>JSON.parse(x[0])).filter(x=>x.type==='personalLibraryRequest').map(x=>x.data);}
it('retains the selected full clipboard text when new history arrives and copies only on user action',()=>{
  expect(nodes.get('libraryPreviewText')!.value).toBe('第一行\n完整多行内容');
  nodes.get('libraryRows')!.children[1].click();expect(nodes.get('libraryPreviewText')!.value).toBe('第二条记录');
  reply(['新复制记录','第一行\n完整多行内容','第二条记录']);
  expect(nodes.get('libraryPreviewText')!.value).toBe('第二条记录');expect(actions()).toEqual([]);
  nodes.get('libraryPreviewActions')!.children.find(x=>x.textContent==='复制')!.click();
  expect(actions()).toMatchObject([{action:'copy',text:'第二条记录'}]);
});
it('saves selected history to an existing group and keeps destructive actions behind confirmation',async()=>{
  nodes.get('libraryPreviewActions')!.children.find(x=>x.textContent==='加入常用语')!.click();
  expect(actions()[0]).toMatchObject({action:'putClip',text:'第一行\n完整多行内容',group:'phrases'});post.mockClear();
  nodes.get('libraryClearHistory')!.click();await Promise.resolve();expect(actions()).toEqual([]);
  confirm.mockResolvedValue(true);nodes.get('libraryClearHistory')!.click();await Promise.resolve();
  expect(actions()).toMatchObject([{action:'clearHistory'}]);
  reply([],false);expect(nodes.get('libraryPreviewText')!.value).toBe('第一行\n完整多行内容');
});
