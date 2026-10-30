import {readFileSync} from 'node:fs';
import {afterEach,beforeEach,expect,it,vi} from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(value:any)=>void>());
const confirm=vi.hoisted(()=>vi.fn());
const toggles=vi.hoisted(()=>new Map<string,(active:boolean)=>void>());
const setupToggle=vi.hoisted(()=>vi.fn((id:string,callback:(active:boolean)=>void)=>toggles.set(id,callback)));
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string,cb:(value:any)=>void)=>handlers.set(type,cb)}));
vi.mock('../utils/confirm-dialog',()=>({confirmDialog:confirm}));
vi.mock('./config-sync',()=>({updateConfig:vi.fn()}));
vi.mock('./shared',()=>({setupToggleButton:setupToggle}));
import {setupPersonalLibrary} from './personal-library';
import {updateConfig} from './config-sync';
class Element extends EventTarget {
  value='';textContent='';hidden=false;disabled=false;checked=false;open=false;type='';id='';className='';scrollTop=0;clientHeight=520;dataset:Record<string,string>={};
  children:Element[]=[];parentElement:Element|null=null;attributes=new Map<string,string>();
  style=Object.assign({} as Record<string,string>,{setProperty:(name:string,value:string)=>{(this.style as any)[name]=value;}});oncontextmenu:((e:any)=>void)|null=null;onkeydown:((e:any)=>void)|null=null;
  classes=new Set<string>();
  classList={toggle:(name:string,force?:boolean)=>{const yes=force??!this.classes.has(name);if(yes)this.classes.add(name);else this.classes.delete(name);},contains:(name:string)=>this.classes.has(name)};
  onclick:((e:Event)=>void)|null=null;onchange:((e:Event)=>void)|null=null;ondblclick:((e:Event)=>void)|null=null;
  isConnected=true;hasAttribute(k:string){return this.attributes.has(k);}
  setAttribute(k:string,v:string){this.attributes.set(k,v);}focus(){(document as any).activeElement=this;}scrollIntoView(){}
  contains(x:Element):boolean{return x===this||this.children.some(c=>c.contains(x));}
  getBoundingClientRect(){return {left:0,bottom:30,width:170,height:108};}
  getClientRects(){return this.hidden?[]:[this.getBoundingClientRect()];}
  querySelector(selector:string){return this.querySelectorAll(selector)[0]??null;}
  append(...nodes:Element[]){for(const node of nodes){node.parentElement=this;this.children.push(node);}}
  replaceChildren(...nodes:Element[]){for(const x of this.children)x.parentElement=null;this.children=[];this.append(...nodes);}
  remove(){if(this.parentElement)this.parentElement.children=this.parentElement.children.filter(x=>x!==this);this.parentElement=null;}
  insertBefore(node:Element,next:Element|null){node.parentElement=this;const index=next?this.children.indexOf(next):-1;if(index<0)this.children.push(node);else this.children.splice(index,0,node);}
  querySelectorAll(selector:string):Element[]{
    if(selector==='[data-library-view]')return this.children.flatMap(x=>[...(x.dataset.libraryView?[x]:[]),...x.querySelectorAll(selector)]);
    if(selector==='[data-quote-source]')return this.children.filter(x=>x.dataset.quoteSource);
    if(selector==='[data-review]')return this.children.filter(x=>x.dataset.review);
    return this.children.filter(x=>selector==='button'||x.className==='clipboard-item');
  }
  override dispatchEvent(e:Event){if(e.type==='change')this.onchange?.(e);return super.dispatchEvent(e);}
  click(){const e=new Event('click');this.onclick?.(e);this.dispatchEvent(e);}
}
let nodes:Map<string,Element>,post:ReturnType<typeof vi.fn>;
let frames:Map<number,FrameRequestCallback>,frameId:number;
function flushFrames(){const pending=[...frames.values()];frames.clear();for(const fn of pending)fn(0);}
function renderedRows(){return nodes.get('libraryRows')!.querySelectorAll('.clipboard-item');}
beforeEach(()=>{
  frames=new Map();frameId=0;handlers.clear();toggles.clear();vi.clearAllMocks();confirm.mockResolvedValue(false);post=vi.fn();
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  nodes=new Map(Array.from(html.matchAll(/id="([^"]+)"/g),m=>[m[1],new Element()]));
  const root=nodes.get('personalLibraryPanel')!;
  for(const m of html.matchAll(/data-library-view="([^"]+)"/g)){const tab=new Element();tab.dataset.libraryView=m[1];root.append(tab);}
  for(const m of html.matchAll(/data-quote-source="([^"]+)"/g)){const tab=new Element();tab.dataset.quoteSource=m[1];root.append(tab);}
  root.append(nodes.get('libraryCustomGroups')!);
  nodes.get('libraryView')!.value='history';
  vi.stubGlobal('Option',class extends Element{constructor(text:string,value:string){super();this.textContent=text;this.value=value;}});
  vi.stubGlobal('document',{getElementById:(id:string)=>nodes.get(id),createElement:()=>new Element(),addEventListener:vi.fn(),activeElement:null,body:new Element()});
  vi.stubGlobal('window',{chrome:{webview:{postMessage:post}},addEventListener:vi.fn(),setInterval:vi.fn(),setTimeout:vi.fn(),innerWidth:887,innerHeight:704,requestAnimationFrame:(fn:FrameRequestCallback)=>{frames.set(++frameId,fn);return frameId;},cancelAnimationFrame:(id:number)=>frames.delete(id)});
  setupPersonalLibrary();
  reply(['第一行\n完整多行内容','第二条记录']);post.mockClear();
});
afterEach(()=>vi.unstubAllGlobals());
function reply(history:string[],ok=true,historyEntries?:{text:string;copies:number}[]){handlers.get('personalLibraryResponse')!({requestId:'library-status-fixture',ok,message:ok?'已读取':'保存失败',history,historyEntries,
  book:{groups:[{id:'phrases',name:'常用语'},{id:'quotes',name:'语录'}],clips:[{id:'saved',name:'旧常用语',text:'保留',group:'phrases'}],rules:[],favorites:[],applications:[],appLearning:[]}});}
function actions(){return post.mock.calls.map(x=>JSON.parse(x[0])).filter(x=>x.type==='personalLibraryRequest').map(x=>x.data);}
function context(index=0){nodes.get('libraryRows')!.children[index].oncontextmenu!({preventDefault(){},clientX:880,clientY:700});return nodes.get('libraryContextMenu')!;}
function selectedText(){return nodes.get('libraryRows')!.children.find(x=>x.attributes.get('aria-selected')==='true')?.children[0].children.find(x=>x.className==='clipboard-item-text')?.textContent;}
it('retains the selected clipboard row when new history arrives and copies only on user action',()=>{
  expect(selectedText()).toBe('第一行\n完整多行内容');
  nodes.get('libraryRows')!.children[1].click();expect(selectedText()).toBe('第二条记录');
  reply(['新复制记录','第一行\n完整多行内容','第二条记录']);
  expect(selectedText()).toBe('第二条记录');expect(actions()).toEqual([]);
  context(2).children.find(x=>x.textContent==='复制')!.click();
  expect(actions()).toMatchObject([{action:'copy',text:'第二条记录'}]);
});
it('saves selected history to an existing group and keeps destructive actions behind confirmation',async()=>{
  context().children.find(x=>x.textContent==='加入常用语')!.click();
  expect(actions()[0]).toMatchObject({action:'putClip',text:'第一行\n完整多行内容',group:'phrases'});post.mockClear();
  nodes.get('libraryClearHistory')!.click();await Promise.resolve();expect(actions()).toEqual([]);
  confirm.mockResolvedValue(true);nodes.get('libraryClearHistory')!.click();await Promise.resolve();
  expect(actions()).toMatchObject([{action:'clearHistory'}]);
  reply([],false);expect(selectedText()).toBe('第一行\n完整多行内容');
});
it('shows actual text and persistent copy counts ordered numerically with stable selection',()=>{
  reply([],true,[{text:'较少复制\n完整内容',copies:2},{text:'最常复制',copies:12},{text:'新内容',copies:1}]);
  const rows=nodes.get('libraryRows')!.children;
  expect(rows.map(row=>row.children[0].children[0].textContent)).toEqual(['最常复制','较少复制\n完整内容','新内容']);
  expect(rows.map(row=>row.children[1].textContent)).toEqual(['+12','+2','+1']);
  rows[1].click();
  reply([],true,[{text:'较少复制\n完整内容',copies:13},{text:'最常复制',copies:12},{text:'新内容',copies:1}]);
  expect(selectedText()).toBe('较少复制\n完整内容');
  expect(actions()).toEqual([]);
});
it('offers explicit context operations, bounds the menu and cancels deletion safely',async()=>{
  const menu=context();expect(menu.hidden).toBe(false);
  expect(menu.children.map(x=>x.textContent)).toEqual(['复制','加入常用语','编辑','删除']);
  expect(menu.style.left).toBe('709px');expect(menu.style.top).toBe('588px');
  expect(actions()).toEqual([]);
  menu.children[3].click();await Promise.resolve();expect(actions()).toEqual([]);
  confirm.mockResolvedValue(true);context().children[3].click();await Promise.resolve();
  expect(actions()).toMatchObject([{action:'removeHistory',text:'第一行\n完整多行内容'}]);
});
it('opens the same menu with the keyboard and closes it on Escape without sending',()=>{
  const row=nodes.get('libraryRows')!.children[1];
  row.onkeydown!({key:'F10',shiftKey:true,preventDefault(){}});
  const menu=nodes.get('libraryContextMenu')!;expect(menu.hidden).toBe(false);
  const event=new Event('keydown',{cancelable:true});Object.defineProperty(event,'key',{value:'Escape'});menu.dispatchEvent(event);
  expect(menu.hidden).toBe(true);expect((document as any).activeElement).toBe(row);expect(actions()).toEqual([]);
});
it('refreshes visible clipboard counts without closing an open context menu or flooding pending requests',()=>{
  const poll=(window.setInterval as any).mock.calls[0][0];
  context();poll();expect(actions()).toEqual([]);
  nodes.get('libraryRows')!.children[0].click();poll();poll();
  expect(actions()).toMatchObject([{action:'history'}]);
});
it('uses the requested left navigation order and suppresses only successful text-copy notifications',()=>{
  expect(nodes.get('personalLibraryPanel')!.children.filter(x=>x.dataset.libraryView).map(x=>x.dataset.libraryView)).toEqual(['history','clips','quotes','favorites','study','rules','learning']);
  handlers.get('personalLibraryResponse')!({requestId:'library-copy-fixture',ok:true,message:'已复制，请自行粘贴；不会自动发送。'});
  expect(nodes.get('libraryErrorText')!.textContent).toBe('');
  handlers.get('personalLibraryResponse')!({requestId:'library-copy-fixture',ok:false,message:'复制失败'});
  expect(nodes.get('libraryErrorText')!.textContent).toBe('复制失败');
});
it('keeps one clipboard switch beside the title and clear immediately before refresh',()=>{
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  const tools=readFileSync(new URL('../partials/tools-settings.html',import.meta.url),'utf8');
  expect(html.match(/id="clipboardHistoryToggleBtn"/g)).toHaveLength(1);
  expect(html).toMatch(/class="clipboard-title-row"><h2>剪贴板<\/h2><div[^>]+id="clipboardHistoryToggleBtn"/);
  expect(html).toMatch(/id="libraryClearHistory"[^>]*>清空<\/button><button id="libraryRefresh"/);
  expect(html).not.toContain('libraryCaptureEnabled');
  expect(tools).not.toContain('clipboardHistoryToggleBtn');
  expect(nodes.get('libraryNewRule')!.hidden).toBe(true);
});
it('reuses the existing clipboard config key and binds the moved switch only once',()=>{
  toggles.get('clipboardHistoryToggleBtn')!(true);
  toggles.get('clipboardHistoryToggleBtn')!(false);
  expect(vi.mocked(updateConfig).mock.calls).toEqual([['utility.clipboard_history',true],['utility.clipboard_history',false]]);
  setupPersonalLibrary();
  expect(setupToggle).toHaveBeenCalledTimes(1);
});
it('shows the complete clipboard list and removes the shared pager markup',()=>{
  reply(Array.from({length:50},(_,i)=>`复制内容 ${i}`));
  expect(nodes.get('libraryRows')!.children).toHaveLength(50);
  expect(nodes.has('libraryPager')).toBe(false);
  expect(nodes.get('libraryRows')!.children[49].children[0].children[0].textContent).toBe('复制内容 49');
});
it.each(['clips','quotes','favorites','rules','learning'])('shows and searches the complete %s list including entries beyond the former last page',view=>{
  const entries=Array.from({length:37},(_,i)=>({id:`entry-${i}`,name:`标题 ${i}`,text:`正文 ${i}`,group:view==='quotes'?'quotes':'phrases',application:'code.exe',code:'ceshi',frequency:i}));
  handlers.get('personalLibraryResponse')!({requestId:'library-status-scroll',ok:true,book:{
    groups:[{id:'phrases',name:'常用语'},{id:'quotes',name:'语录'}],clips:['clips','quotes'].includes(view)?entries:[],
    favorites:view==='favorites'?entries:[],rules:view==='rules'?entries:[],appLearning:view==='learning'?entries:[],applications:[]}});
  nodes.get('libraryRows')!.scrollTop=500;
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView===view)!.click();
  expect(nodes.get('libraryRows')!.scrollTop).toBe(0);
  if(view==='quotes')nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.quoteSource==='custom')!.click();
  expect(nodes.get('libraryRows')!.children).toHaveLength(37);
  expect(nodes.get('libraryRows')!.children.at(-1)!.children[0].children[1].textContent).toBe('正文 36');
  expect(nodes.get('libraryResultCount')!.textContent).toBe('共 37 条');
  nodes.get('libraryRows')!.scrollTop=500;
  const search=nodes.get('librarySearch')!;search.value='正文 36';(search as any).oninput();
  expect(nodes.get('libraryRows')!.scrollTop).toBe(0);
  expect(nodes.get('libraryRows')!.children).toHaveLength(1);
  post.mockClear();context().children.find(x=>x.textContent===(view==='favorites'?'播放':'复制'))!.click();
  expect(actions()).toMatchObject(view==='favorites'?[{action:'playFavorite',id:'entry-36'}]:[{action:'copy',text:'正文 36'}]);
  search.value='';(search as any).oninput();expect(nodes.get('libraryRows')!.children).toHaveLength(37);
});
it('hides saved-to-local acknowledgements and shows errors after the hidden status',()=>{
  const response=handlers.get('personalLibraryResponse')!;
  response({requestId:'library-putClip-fixture',ok:true,message:'已保存到本机'});
  expect(nodes.get('libraryError')!.hidden).toBe(true);
  expect(nodes.get('libraryErrorText')!.textContent).toBe('');
  response({requestId:'library-putGroup-fixture',ok:true});
  expect(nodes.get('libraryError')!.hidden).toBe(true);
  response({requestId:'library-putClip-fixture',ok:false,message:'磁盘写入失败'});
  expect(nodes.get('libraryError')!.hidden).toBe(false);
  expect(nodes.get('libraryErrorText')!.textContent).toBe('磁盘写入失败');
});
it('removes routine status rows in every collection but retains a separate failure alert',()=>{
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  expect(html).not.toContain('libraryStatus');
  expect(html).not.toContain('<summary>编辑内容</summary>');
  expect(html).not.toContain('新建常用语');
  expect(html).toMatch(/id="libraryNew"[^>]*>新建<\/button><button id="libraryAddGroup"/);
  const root=nodes.get('personalLibraryPanel')!;
  for(const tab of root.children.filter(x=>x.dataset.libraryView)){
    tab.click();reply(['已有记录']);
    expect(nodes.get('libraryError')!.hidden).toBe(true);
    expect(nodes.get('libraryErrorText')!.textContent).toBe('');
  }
});
it('edits clipboard text in a modal without copying, keeps input after failure and closes after persistence',()=>{
  const row=nodes.get('libraryRows')!.children[0];row.ondblclick!(new Event('dblclick'));
  expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(true);
  expect(nodes.get('libraryText')!.value).toBe('第一行\n完整多行内容');
  expect(actions()).toEqual([]);
  const poll=(window.setInterval as any).mock.calls[0][0];poll();expect(actions()).toEqual([]);
  nodes.get('libraryText')!.value='编辑后的内容';nodes.get('librarySave')!.click();
  expect(actions()[0]).toMatchObject({action:'editHistory',previousText:'第一行\n完整多行内容',text:'编辑后的内容'});
  handlers.get('personalLibraryResponse')!({requestId:actions()[0].requestId,ok:false,message:'磁盘无法写入'});
  expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(true);
  expect(nodes.get('libraryText')!.value).toBe('编辑后的内容');
  post.mockClear();nodes.get('librarySave')!.click();
  handlers.get('personalLibraryResponse')!({requestId:actions()[0].requestId,ok:true,message:'',historyEntries:[{text:'编辑后的内容',copies:6}]});
  expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(false);
  expect(nodes.get('libraryRows')!.children[0].children[1].textContent).toBe('+6');
});
it('quotes has real builtin collections, lazily loads poetry and saves editable copies to custom quotes',()=>{
  const root=nodes.get('personalLibraryPanel')!;
  root.children.find(x=>x.dataset.libraryView==='quotes')!.click();
  expect(actions()).toMatchObject([{action:'quoteCatalog'}]);
  expect(nodes.get('libraryResultCount')!.textContent).toBe('共 60 条');
  handlers.get('personalLibraryResponse')!({requestId:actions()[0].requestId,ok:true,message:'',quoteEntries:[{id:'poetry-1',name:'青简 · 诗词名句',text:'海内存知己'}]});
  root.children.find(x=>x.dataset.quoteSource==='poetry')!.click();
  expect(nodes.get('libraryResultCount')!.textContent).toBe('共 1 条');
  const row=nodes.get('libraryRows')!.children[0];row.ondblclick!(new Event('dblclick'));
  expect(nodes.get('librarySave')!.textContent).toBe('保存为自定义语录');
  nodes.get('libraryText')!.value='我的自定义语录';post.mockClear();nodes.get('librarySave')!.click();
  expect(actions()[0]).toMatchObject({action:'putClip',group:'quotes',text:'我的自定义语录'});
  expect(actions()[0].id).not.toBe('poetry-1');
});
it('cancelling a new phrase does not persist or erase existing records',()=>{
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='clips')!.click();
  nodes.get('libraryNew')!.click();nodes.get('libraryText')!.value='未保存草稿';nodes.get('libraryCancelEdit')!.click();
  expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(false);
  expect(actions()).toEqual([]);
  expect(nodes.get('libraryRows')!.children[0].children[0].children[0].textContent).toBe('旧常用语');
});
it('does not replace row nodes on an unchanged poll so double clicks and focus stay reliable',()=>{
  const row=nodes.get('libraryRows')!.children[0];
  handlers.get('personalLibraryResponse')!({requestId:'library-history-stable',ok:true,message:'',historyEntries:[{text:'第一行\n完整多行内容',copies:1},{text:'第二条记录',copies:1}]});
  expect(nodes.get('libraryRows')!.children[0]).toBe(row);
  row.ondblclick!(new Event('dblclick'));expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(true);
});
it('marks exactly the current main view or custom group as active after every navigation',()=>{
  const root=nodes.get('personalLibraryPanel')!;
  const active=()=>root.querySelectorAll('[data-library-view]').filter(x=>x.attributes.get('aria-pressed')==='true');
  expect(active().map(x=>x.dataset.libraryView)).toEqual(['history']);
  for(const view of ['clips','quotes','favorites','study','rules','learning','history']){
    const tab=root.children.find(x=>x.dataset.libraryView===view)!;tab.click();expect(active()).toEqual([tab]);
  }
  handlers.get('personalLibraryResponse')!({requestId:'library-status-groups',ok:true,book:{
    groups:[{id:'phrases',name:'常用语'},{id:'quotes',name:'语录'},{id:'work',name:'工作'},{id:'inspiration',name:'灵感',kind:'quotes'}],
    clips:[],rules:[],favorites:[],applications:[],appLearning:[]}});
  for(const group of ['work','inspiration']){
    const tab=nodes.get('libraryCustomGroups')!.children.find(x=>x.dataset.libraryGroup===group)!;tab.click();
    expect(active()).toHaveLength(1);expect(active()[0].dataset.libraryGroup).toBe(group);
  }
  root.children.find(x=>x.dataset.libraryView==='quotes')!.click();
  expect(active().map(x=>x.dataset.libraryGroup??x.dataset.libraryView)).toEqual(['quotes']);
});
it('removes the preview panel while retaining explicit favorite playback and audio copy',()=>{
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  expect(html).not.toContain('libraryPreview');expect(html).not.toContain('clipboard-preview');
  handlers.get('personalLibraryResponse')!({requestId:'library-status-favorite',ok:true,book:{
    groups:[{id:'phrases',name:'常用语'},{id:'quotes',name:'语录'}],clips:[],rules:[],
    favorites:[{id:'voice-1',name:'问候',text:'你好，欢迎回来。'}],applications:[],appLearning:[]}});
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='favorites')!.click();
  nodes.get('libraryRows')!.children[0].click();expect(actions()).toEqual([]);
  expect(context().children.map(x=>x.textContent)).toEqual(['播放','复制音频文件','删除']);
  context().children.find(x=>x.textContent==='播放')!.click();
  expect(actions()).toMatchObject([{action:'playFavorite',id:'voice-1'}]);post.mockClear();
  context().children.find(x=>x.textContent==='复制音频文件')!.click();
  expect(actions()).toMatchObject([{action:'copyFavorite',id:'voice-1'}]);
});
it('places new software entries before the software switch save action and keeps the selected scope',()=>{
  const html=readFileSync(new URL('../partials/personal-library.html',import.meta.url),'utf8');
  expect(html).toMatch(/id="libraryNewRule"[^>]*>新建软件词条<\/button><button id="librarySaveApplication"/);
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='rules')!.click();
  expect(nodes.get('libraryNewRule')!.hidden).toBe(false);
  nodes.get('libraryScope')!.value='Code.exe';nodes.get('libraryScope')!.dispatchEvent(new Event('change'));
  nodes.get('libraryNewRule')!.click();
  expect(nodes.get('libraryEditor')!.classes.has('open')).toBe(true);
  expect(nodes.get('libraryAppFields')!.hidden).toBe(false);expect(nodes.get('libraryApplication')!.value).toBe('Code.exe');
  expect(actions()).toEqual([]);
  nodes.get('libraryCancelEdit')!.click();
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='learning')!.click();
  expect(nodes.get('libraryNewRule')!.hidden).toBe(true);
});

function largeCatalog(){
  const entries=Array.from({length:13446},(_,i)=>({id:`poetry-${i}`,name:'诗词名句',text:`诗句正文 ${i}`}));
  handlers.get('personalLibraryResponse')!({requestId:'library-quoteCatalog-large',ok:true,quoteEntries:entries});
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='quotes')!.click();
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.quoteSource==='poetry')!.click();
}
it('bounds the live rows while all 13446 poems remain reachable and searchable',()=>{
  largeCatalog();const list=nodes.get('libraryRows')!;
  expect(nodes.get('libraryResultCount')!.textContent).toBe('共 13446 条');
  expect(renderedRows().length).toBeLessThanOrEqual(49);
  expect(renderedRows()[0].attributes.get('aria-setsize')).toBe('13446');
  expect(list.style['--library-row-height']).toBe('70px');
  expect(renderedRows()[0].children[0].children).toHaveLength(2);
  for(const index of [1000,8000,50,13442]){list.scrollTop=index*70;list.dispatchEvent(new Event('scroll'));flushFrames();expect(renderedRows().length).toBeLessThanOrEqual(49);}
  expect(renderedRows().at(-1)!.attributes.get('aria-posinset')).toBe('13446');
  expect(renderedRows().at(-1)!.children[0].children[1].textContent).toBe('诗句正文 13445');
  const search=nodes.get('librarySearch')!;search.value='诗句正文 13445';(search as any).oninput();
  expect(renderedRows()).toHaveLength(1);expect(list.scrollTop).toBe(0);
  context().children.find(x=>x.textContent==='复制')!.click();expect(actions().at(-1)).toMatchObject({action:'copy',text:'诗句正文 13445'});
});
it('cancels old scroll work when switching categories and does not paint the previous list',()=>{
  largeCatalog();const list=nodes.get('libraryRows')!;
  list.scrollTop=10000;list.dispatchEvent(new Event('scroll'));expect(frames.size).toBe(1);
  const stale=[...frames.values()][0];
  nodes.get('personalLibraryPanel')!.children.find(x=>x.dataset.libraryView==='clips')!.click();
  stale(0);flushFrames();
  expect(renderedRows()).toHaveLength(1);expect(renderedRows()[0].children[0].children[0].textContent).toBe('旧常用语');
  expect(list.classes.has('is-virtual')).toBe(false);
});
it('navigates across virtual windows with arrows, Home and End and edits the correct last row',()=>{
  largeCatalog();
  const key=(row:Element,key:string)=>row.onkeydown!({key,preventDefault(){}});
  key(renderedRows()[0],'End');
  let last=(document as any).activeElement as Element;
  expect(last.attributes.get('aria-posinset')).toBe('13446');
  expect(last.attributes.get('aria-selected')).toBe('true');
  key(last,'ArrowUp');expect(((document as any).activeElement as Element).attributes.get('aria-posinset')).toBe('13445');
  key((document as any).activeElement,'ArrowDown');last=(document as any).activeElement;
  last.ondblclick!(new Event('dblclick'));expect(nodes.get('libraryText')!.value).toBe('诗句正文 13445');
  nodes.get('libraryCancelEdit')!.click();
  key(last,'Home');expect(((document as any).activeElement as Element).attributes.get('aria-posinset')).toBe('1');
  key((document as any).activeElement,'ArrowUp');expect(((document as any).activeElement as Element).attributes.get('aria-posinset')).toBe('1');
  expect(renderedRows().length).toBeLessThanOrEqual(49);
});
it('retains a focused offscreen row without accumulating all previous windows',()=>{
  largeCatalog();const list=nodes.get('libraryRows')!;const first=renderedRows()[0];first.focus();
  for(const index of [1000,2000,4000]){list.scrollTop=index*70;list.dispatchEvent(new Event('scroll'));flushFrames();}
  expect(renderedRows()).toContain(first);expect((document as any).activeElement).toBe(first);
  expect(renderedRows().length).toBeLessThanOrEqual(49);
  nodes.get('librarySearch')!.focus();list.scrollTop=8000*70;list.dispatchEvent(new Event('scroll'));flushFrames();
  expect(renderedRows()).not.toContain(first);
  const positions=renderedRows().map(row=>Number(row.attributes.get('aria-posinset')));
  expect(positions).toEqual([...positions].sort((a,b)=>a-b));
});
