import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import "../styles/modules/prompt-library.css";
import "../styles/modules/workbench-layout.css";
import "../styles/modules/clipboard.css";
import {confirmDialog} from '../utils/confirm-dialog';
import {updateConfig} from './config-sync';
import {setupToggleButton} from './shared';
import {BUILTIN_QUOTES,groupIsQuotes} from './quote-library';
import {LIBRARY_COMPACT_ROW_HEIGHT,VirtualLibraryList} from './virtual-library-list';
type Action =
  | "status"
  | "history"
  | "editHistory"
  | "quoteCatalog"
  | "putGroup"
  | "removeGroup"
  | "putClip"
  | "putRule"
  | "favorite"
  | "remove"
  | "copy"
  | "review"
  | "playFavorite"
  | "copyFavorite"
  | "study"
  | "putApplication"
  | "exportApplication"
  | "importApplication"
  | "removeLearning"
  | "removeHistory"
  | "clearHistory";
const el = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const value = (id: string) => el<HTMLInputElement>(id)?.value ?? "";
export function personalAction(
  action: Action,
  extra: Record<string, unknown> = {},
): string {
  const requestId = `library-${action}-${Date.now()}-${Math.random()}`;
  window.chrome?.webview?.postMessage(
    serializeHostMessage({
      type: "personalLibraryRequest",
      data: {
        requestId,
        action,
        ...extra,
      },
    }),
  );
  return requestId;
}
type Entry = {
  id: string;
  name: string;
  text?: string;
  group?: string;
  application?: string;
  code?: string;
  enabled?: boolean;
  language?: string;
  frequency?: number;
  deleted?: boolean;
  pinned?: boolean;
  copies?: number;
  builtin?: boolean;
  quoteSource?: string;
};
export function filterLibraryEntries(entries: Entry[], query: string): Entry[] {
  const needle=query.trim().toLocaleLowerCase();
  return needle?entries.filter(x=>[x.name,x.text,x.application,x.code].some(value=>value?.toLocaleLowerCase().includes(needle))):entries;
}
type Book = {
  groups: { id: string; name: string; kind?: string }[];
  clips: Entry[];
  rules: Entry[];
  favorites: Entry[];
  applications: {application:string;enabled:boolean;learn:boolean;frequency:boolean}[];
  appLearning: Entry[];
};
export function setupPersonalLibrary(): void {
  const root = document.getElementById("personalLibraryPanel");
  if (!root || root.dataset.ready) return;
  root.dataset.ready = "1";
  setupToggleButton('clipboardHistoryToggleBtn', active => updateConfig('utility.clipboard_history', active));
  let book: Book = { groups: [], clips: [], rules: [], favorites: [], applications:[],appLearning:[] },
    history: {text:string;copies:number}[] = [],
    editing = "", selectedKey = "";
  let activeGroup='phrases',quoteSource='all',poetry:Entry[]=[],poetryRequested=false;
  let editorKind:'history'|'clips'|'quotes'|'rules'='clips',previousText='',editorRequest='';
  let editorFocus:HTMLElement|null=null;
  let study: { word: string; language: string; gloss: string }[] = [],
    reviewed = 0;
  const status = (s: string) => {
    el("libraryErrorText").textContent = s;
    el("libraryError").hidden = !s;
  };
  const button = (name: string, action: () => void) => {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = name;
    b.addEventListener("click", action);
    return b;
  };
  const menu = el('libraryContextMenu');
  document.body.append(menu);
  const editor=el('libraryEditor');document.body.append(editor);
  const editorError=(text:string)=>{el('libraryEditorError').textContent=text;el('libraryEditorError').hidden=!text;};
  const closeEditor=()=>{
    if(editorRequest)return;
    editor.classList.toggle('open',false);editor.setAttribute('aria-hidden','true');
    (editorFocus?.isConnected?editorFocus:el('librarySearch')).focus();
  };
  const openEditor=(kind:typeof editorKind,x?:Entry)=>{
    if(editorRequest)return;
    closeMenu();editorKind=kind;editorFocus=document.activeElement as HTMLElement|null;
    editing=x?.builtin?'':x?.id??'';previousText=kind==='history'?x?.text??'':'';
    el<HTMLInputElement>('libraryName').value=x?.name??'';
    el<HTMLTextAreaElement>('libraryText').value=x?.text??'';
    el<HTMLInputElement>('libraryApplication').value=x?.application??value('libraryScope').trim();
    el<HTMLInputElement>('libraryCode').value=x?.code??'';
    el<HTMLInputElement>('libraryEnabled').checked=x?.enabled!==false;
    el('libraryTitleFields').hidden=kind==='history';el('libraryAppFields').hidden=kind!=='rules';
    el<HTMLTextAreaElement>('libraryText').maxLength=kind==='history'?4000:kind==='rules'?2000:16384;
    const label=kind==='history'?'剪贴内容':kind==='quotes'?'语录':kind==='rules'?'软件词条':'常用语';
    el('libraryEditorTitle').textContent=(x?'编辑':'新建')+label;
    el('librarySave').textContent=x?.builtin?'保存为自定义语录':'保存';
    editorError('');editor.classList.toggle('open',true);editor.setAttribute('aria-hidden','false');
    el(kind==='history'?'libraryText':'libraryName').focus();
  };
  el('libraryCancelEdit').onclick=closeEditor;
  el('libraryDismissError').onclick=()=>status('');
  editor.addEventListener('keydown',event=>{
    const e=event as KeyboardEvent;
    if(e.key==='Escape'){e.preventDefault();closeEditor();}
    if(e.key==='Enter'&&(e.ctrlKey||e.metaKey)){e.preventDefault();el('librarySave').click();}
    if(e.key==='Tab'){
      const fields=Array.from(editor.querySelectorAll<HTMLElement>('input:not([type="hidden"]),textarea,button')).filter(x=>!x.hasAttribute('disabled')&&x.getClientRects().length);
      const first=fields[0],last=fields[fields.length-1];
      if(e.shiftKey&&document.activeElement===first){e.preventDefault();last?.focus();}
      else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first?.focus();}
    }
  });
  let menuOrigin: HTMLElement | null = null;
  let historyPending=false, historyRequestedAt=0;
  const refreshHistory=()=>{
    if(value('libraryView')!=='history'||!menu.hidden||editor.classList.contains('open')||document.hidden||!root.getClientRects().length)return;
    if(historyPending&&Date.now()-historyRequestedAt<5000)return;
    historyPending=true;historyRequestedAt=Date.now();personalAction('history');
  };
  window.setInterval(refreshHistory,1000);
  window.addEventListener('focus',refreshHistory);
  const closeMenu = (restoreFocus = false) => {
    menu.hidden = true;
    if (restoreFocus) menuOrigin?.focus();
    menuOrigin = null;
  };
  document.addEventListener('pointerdown', event => { if (!menu.contains(event.target as Node)) closeMenu(); });
  document.addEventListener('click', event => { if (!menu.contains(event.target as Node)) closeMenu(); });
  document.addEventListener('scroll', () => closeMenu(), true);
  window.addEventListener('blur', () => closeMenu());
  menu.addEventListener('keydown', event => {
    const key = (event as KeyboardEvent).key;
    if (key === 'Escape') { event.preventDefault(); closeMenu(true); return; }
    if (!['ArrowDown','ArrowUp','Home','End'].includes(key)) return;
    event.preventDefault();
    const buttons = Array.from(menu.querySelectorAll<HTMLButtonElement>('button'));
    const index = buttons.indexOf(document.activeElement as HTMLButtonElement);
    const next = key === 'Home' ? 0 : key === 'End' ? buttons.length - 1 : (index + (key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
    buttons[next]?.focus();
  });
  const reset = () => {
    editing = "";
    for (const id of ["libraryName", "libraryText", "libraryCode"])
      el<HTMLInputElement>(id).value = "";
    el<HTMLInputElement>("libraryEnabled").checked = true;
  };
  const listView = new VirtualLibraryList<Entry>(el('libraryRows'));
  const render = () => {
    closeMenu();
    const view = value("libraryView"),
      isClip = view === "clips",
      isRule = view === "rules",isQuote=view==='quotes',isCompact=view!=='history'&&view!=='study';
    if(view==='clips'||isQuote){
      const valid=book.groups.some(g=>g.id===activeGroup&&groupIsQuotes(g)===isQuote);
      if(!valid)activeGroup=isQuote?'quotes':'phrases';
      el<HTMLInputElement>('libraryGroup').value=activeGroup;
    }
    const groupNav=el('libraryCustomGroups');groupNav.replaceChildren();
    for(const group of book.groups.filter(g=>!['phrases','quotes'].includes(g.id))){
      const tab=button(`${groupIsQuotes(group)?'语录':'常用语'} · ${group.name}`,()=>{
        activeGroup=group.id;quoteSource=groupIsQuotes(group)?'custom':'all';
        el<HTMLSelectElement>('libraryView').value=groupIsQuotes(group)?'quotes':'clips';
        el<HTMLInputElement>('libraryGroupName').value=group.name;
        el<HTMLInputElement>('librarySearch').value='';renderFromStart();
      });
      tab.dataset.libraryView=groupIsQuotes(group)?'quotes':'clips';tab.dataset.libraryGroup=group.id;
      groupNav.append(tab);
    }
    root.querySelectorAll<HTMLButtonElement>('[data-library-view]').forEach(tab=>tab.setAttribute('aria-pressed',String(tab.dataset.libraryView===view&&(tab.dataset.libraryGroup?tab.dataset.libraryGroup===activeGroup:!['clips','quotes'].includes(view)||activeGroup===(view==='quotes'?'quotes':'phrases')))));
    el('libraryBrowse').hidden=view==='study';
    el('libraryNewRule').hidden=!isRule;
    el('libraryClearHistory').hidden=view!=='history';
    el<HTMLButtonElement>('libraryClearHistory').disabled=!history.length;
    el("libraryGroups").hidden = !isClip&&!isQuote;
    const top = document.getElementById("libraryNewTop"); if (top) top.hidden = !isClip&&!isQuote;
    const fixedGroup=['phrases','quotes'].includes(activeGroup);
    el<HTMLButtonElement>('libraryRenameGroup').disabled=fixedGroup;
    el<HTMLButtonElement>('libraryRemoveGroup').disabled=fixedGroup;
    el('libraryQuoteSources').hidden=!isQuote;
    root.querySelectorAll<HTMLButtonElement>('[data-quote-source]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.quoteSource===quoteSource)));
    if(isQuote&&!poetryRequested){poetryRequested=true;personalAction('quoteCatalog');}
    el('libraryQuoteSourceNote').textContent=`经典语录 30 条 · 名人语录 30 条 · 青简诗词名句 ${poetry.length} 条；作者与出处见标题，自定义语录保存在本机。`;
    el("libraryApplicationManager").hidden = !isRule && view !== "learning";
    const appSelect=el<HTMLSelectElement>("libraryApplicationSelect");
    const app=value("libraryScope").trim().toLowerCase();
    appSelect.replaceChildren(new Option("选择或填写新软件", ""),...(book.applications??[]).map(x=>new Option(x.application+(x.enabled?" · 已启用":" · 已关闭"),x.application)));
    appSelect.value=app;
    const profile=(book.applications??[]).find(x=>x.application===app);
    if(profile){el<HTMLInputElement>("libraryIsolation").checked=profile.enabled;el<HTMLInputElement>("libraryAutoLearn").checked=profile.learn;el<HTMLInputElement>("libraryFrequency").checked=profile.frequency;}
    el("libraryStudy").hidden = view !== "study";
    el("librarySearchArea").hidden = view === "study";
    const rows = el("libraryRows");
    rows.classList.toggle('is-compact',isCompact);
    if (view === "study") {listView.clear();return;}
    const entries: Entry[] =
      view === "history"
        ? history.map((x, i) => ({id:String(i),name:'',text:x.text,copies:x.copies}))
        : view === "clips"
          ? book.clips.filter((x) => x.group === activeGroup)
          : isQuote
            ? [
                ...(quoteSource==='all'||quoteSource==='custom'?book.clips.filter(x=>x.group===activeGroup):[]),
                ...(quoteSource==='all'||quoteSource==='classic'||quoteSource==='famous'?BUILTIN_QUOTES.filter(x=>quoteSource==='all'||x.quoteSource===quoteSource):[]),
                ...(quoteSource==='all'||quoteSource==='poetry'?poetry:[]),
              ]
          : view === "rules"
            ? book.rules.filter(x=>!app||x.application===app)
            : view === "learning"
              ? (book.appLearning??[]).filter(x=>!app||x.application===app).map(x=>({...x,name:`${x.language??"zh"} · ${x.frequency??0} 次${x.deleted?' · 本软件已隐藏':''}${x.pinned?' · 本软件置顶':''}`}))
              : book.favorites;
    const visible=filterLibraryEntries(entries,value("librarySearch"));
    el("libraryResultCount").textContent=`共 ${visible.length} 条`;
    const entryKey=(x:Entry)=>`${view}:${view==='history'?x.text:x.id}`;
    if(!visible.some(x=>entryKey(x)===selectedKey))selectedKey=visible[0]?entryKey(visible[0]):'';
    const openMenu = (x:Entry, row:HTMLButtonElement, clientX:number, clientY:number) => {
      menuOrigin = row;
      menu.replaceChildren();
      const addAction = (label:string, action:()=>void) => {
        const item=button(label,()=>{closeMenu(true);action();});item.setAttribute('role','menuitem');menu.append(item);
      };
      if(view==='favorites') {
        addAction('播放',()=>personalAction('playFavorite',{id:x.id}));
        addAction('复制音频文件',()=>personalAction('copyFavorite',{id:x.id}));
      }
      else addAction('复制',()=>personalAction('copy',{text:x.text??''}));
      if(view==='history') addAction('加入常用语',()=>personalAction('putClip',{
        name:(x.text??'').slice(0,30)||'收藏文本',text:x.text,
        group:book.groups.find(g=>g.id==='phrases')?.id??book.groups[0]?.id
      }));
      if(['history','clips','quotes','rules'].includes(view))addAction(x.builtin?'编辑并收藏':'编辑',()=>openEditor(view as typeof editorKind,x));
      if(!x.builtin)addAction('删除',async()=>{
        if(!await confirmDialog(view==='history'?'删除这条内容？保存的常用语和其他资料不会删除。':'删除这条已保存的内容？'))return;
        if(view==='history')personalAction('removeHistory',{text:x.text});
        else if(view==='learning')personalAction('removeLearning',{application:x.application,id:x.id});
        else personalAction('remove',{section:view==='quotes'?'clips':view,id:x.id});
      });
      menu.hidden=false;
      const bounds=menu.getBoundingClientRect();
      menu.style.left=`${Math.max(8,Math.min(clientX,window.innerWidth-bounds.width-8))}px`;
      menu.style.top=`${Math.max(8,Math.min(clientY,window.innerHeight-bounds.height-8))}px`;
      menu.querySelector<HTMLButtonElement>('button')?.focus();
    };
    listView.setItems(visible,(x,index)=>{
      const row=document.createElement('button');row.type='button';row.className='clipboard-item';row.setAttribute('role','option');
      row.setAttribute('aria-haspopup','menu');
      const key=entryKey(x),selected=key===selectedKey;row.setAttribute('aria-selected',String(selected));
      const main=document.createElement('span');main.className='clipboard-item-main';
      if(view==='history'){
        const text=document.createElement('span');text.className='clipboard-item-text';text.textContent=x.text??'';
        main.append(text);const count=document.createElement('small');count.className='clipboard-copy-count';
        count.textContent=`+${x.copies??1}`;count.setAttribute('aria-label',`已复制 ${x.copies??1} 次`);row.append(main,count);
      }else{
        const title=document.createElement('strong'),snippet=document.createElement('span');
        title.textContent=x.name;snippet.className='clipboard-item-text';snippet.textContent=x.text??'';
        const details=!isClip&&!isQuote?[x.enabled===false?'已停用':'',x.application,x.code].filter(Boolean).join(' · '):'';
        if(details){
          const heading=document.createElement('span'),meta=document.createElement('small');
          heading.className='clipboard-item-heading';meta.textContent=details;meta.title=details;
          heading.append(title,meta);main.append(heading,snippet);
        }else main.append(title,snippet);
        row.append(main);
      }
      row.onclick=()=>{closeMenu();selectedKey=key;rows.querySelectorAll<HTMLButtonElement>('.clipboard-item').forEach(b=>b.setAttribute('aria-selected',String(b===row)));};
      if(['history','clips','quotes','rules'].includes(view))row.ondblclick=()=>openEditor(view as typeof editorKind,x);
      row.oncontextmenu=e=>{e.preventDefault();row.click();openMenu(x,row,e.clientX,e.clientY);};
      row.onkeydown=e=>{
        if(e.key==='ContextMenu'||e.key==='F10'&&e.shiftKey){e.preventDefault();row.click();const bounds=row.getBoundingClientRect();openMenu(x,row,bounds.left+12,bounds.bottom);return;}
        if(!['ArrowDown','ArrowUp','Home','End'].includes(e.key))return;
        e.preventDefault();
        listView.focusRow(e.key==='Home'?0:e.key==='End'?visible.length-1:index+(e.key==='ArrowDown'?1:-1));
      };
      return row;
    },isCompact?LIBRARY_COMPACT_ROW_HEIGHT:undefined);
    if (!visible.length) {
      const p = document.createElement("p");
      p.textContent =
        view === "favorites"
          ? "在语音工作台生成后点击“收藏语音”，即可在此重复播放或复制。"
          : "暂无内容。";
      rows.append(p);
    }
  };
  const renderFromStart = () => {
    el('libraryRows').scrollTop=0;render();root.scrollIntoView({block:'start'});
  };
  const renderStudy = () => {
    const x = study[0];
    el("libraryStudyWord").textContent = x
      ? `${x.word} · ${x.language}`
      : "本轮复习完成或暂无记录";
    el("libraryStudyAnswer").textContent = x?.gloss ?? "";
    el("libraryStudyAnswer").hidden = true;
    el("libraryStudyCount").textContent =
      `本轮已复习 ${reviewed} 条 · 剩余 ${study.length} 条`;
    root
      .querySelectorAll<HTMLButtonElement>("[data-review]")
      .forEach((b) => (b.disabled = !x));
  };
  const applyScope=()=>{
    el<HTMLInputElement>("libraryApplication").value=value("libraryScope").trim();
    reset();el('libraryRows').scrollTop=0;render();
  };
  el("libraryApplicationSelect").onchange=()=>{el<HTMLInputElement>("libraryScope").value=value("libraryApplicationSelect");applyScope();};
  el("libraryScope").onchange=applyScope;
  el("librarySaveApplication").onclick=()=>personalAction("putApplication",{
    application:value("libraryScope").trim(),enabled:el<HTMLInputElement>("libraryIsolation").checked,
    learn:el<HTMLInputElement>("libraryAutoLearn").checked,frequency:el<HTMLInputElement>("libraryFrequency").checked,
  });
  el("libraryExportApplication").onclick=()=>personalAction("exportApplication",{application:value("libraryScope").trim()});
  const importFile=el<HTMLInputElement>("libraryImportFile");
  el("libraryImportApplication").onclick=()=>{
    if(!value("libraryScope").trim()){status("请先填写导入目标软件文件名");return;}importFile.click();
  };
  importFile.onchange=async()=>{
    try {const file=importFile.files?.[0];if(!file)return;if(file.size>4*1024*1024)throw Error("应用词库文件限4MB");
      personalAction("importApplication",{application:value("libraryScope").trim(),payload:(await file.text()).replace(/^\uFEFF/,"")});
    }catch(error){status(String(error));}finally{importFile.value="";}
  };
  root.querySelectorAll<HTMLButtonElement>('[data-library-view]').forEach(tab=>tab.onclick=()=>{
    activeGroup=tab.dataset.libraryView==='quotes'?'quotes':'phrases';quoteSource='all';
    el<HTMLSelectElement>('libraryView').value=tab.dataset.libraryView??'history';el('libraryView').dispatchEvent(new Event('change'));
  });
  el('libraryClearHistory').onclick=async()=>{
    if(await confirmDialog('清空全部复制记录？常用语、分组、语音收藏和词库不受影响。',{title:'清空复制记录',confirmText:'清空'}))personalAction('clearHistory');
  };
  window.chrome?.webview?.postMessage(serializeHostMessage({type:'configRequest'}));
  el("libraryRefresh").onclick = () => personalAction("status");
  el("libraryView").onchange = () => {
    el<HTMLInputElement>("librarySearch").value="";
    reset();
    status('');
    renderFromStart();
  };
  document.getElementById("libraryNewTop")?.addEventListener("click", () => el("libraryNew").click());
  el("libraryNew").onclick = ()=>openEditor(value('libraryView')==='quotes'?'quotes':'clips');
  el('libraryNewRule').onclick=()=>openEditor('rules');
  root.querySelectorAll<HTMLButtonElement>('[data-quote-source]').forEach(tab=>tab.onclick=()=>{quoteSource=tab.dataset.quoteSource??'all';el('libraryRows').scrollTop=0;render();});
  el("librarySearch").oninput=()=>{el('libraryRows').scrollTop=0;render();};
  el("libraryAddGroup").onclick = () =>
    personalAction("putGroup", { name: value("libraryGroupName").trim(),section:value('libraryView')==='quotes'?'quotes':'clips' });
  el("libraryRenameGroup").onclick = () =>
    personalAction("putGroup", {
      id: value("libraryGroup"),
      name: value("libraryGroupName").trim(),
    });
  el("libraryRemoveGroup").onclick = async() => {
    if(await confirmDialog('删除此自定义分组并转移其中内容？资料本身会保留。',{title:'删除分组',confirmText:'删除并转移'}))personalAction("removeGroup", { id: activeGroup });
  };
  el("librarySave").onclick = () => {
    if(editorRequest)return;
    const text=value('libraryText');
    if(!text.trim()){editorError('请填写文字内容');return;}
    if(editorKind!=='history'&&!value('libraryName').trim()){editorError('请填写标题');return;}
    const rule = editorKind === "rules";
    // Keep the identity across repeated saves, including a failed validation retry.
    if (!editing) editing = crypto.randomUUID();
    editorError('');
    editorRequest=editorKind==='history'?personalAction('editHistory',{previousText,text}):personalAction(rule ? "putRule" : "putClip", {
      id: editing,
      name: value("libraryName").trim(),
      text: value("libraryText"),
      ...(rule
        ? {
            application: value("libraryApplication").trim(),
            code: value("libraryCode").trim(),
            enabled: el<HTMLInputElement>("libraryEnabled").checked,
          }
        : { group: activeGroup }),
    });
    el<HTMLButtonElement>('librarySave').disabled=true;
    const savedRequest=editorRequest;
    window.setTimeout(()=>{
      if(editorRequest===savedRequest){editorRequest='';el<HTMLButtonElement>('librarySave').disabled=false;editorError('保存响应超时，请刷新后确认；填写内容仍保留。');}
    },15000);
  };
  el("libraryLoadStudy").onclick = () => {
    reviewed = 0;
    personalAction("study");
  };
  el("libraryReveal").onclick = () => {
    el("libraryStudyAnswer").hidden = false;
  };
  root.querySelectorAll<HTMLButtonElement>("[data-review]").forEach(
    (b) =>
      (b.onclick = () => {
        const x = study[0];
        if (!x) return;
        personalAction("review", {
          text: x.word,
          language: x.language,
          grade: Number(b.dataset.review),
        });
        root
          .querySelectorAll<HTMLButtonElement>("[data-review]")
          .forEach((b) => (b.disabled = true));
      }),
  );
  onHostMessage("personalLibraryResponse", (message) => {
    if (!message.requestId.startsWith("library-")) return;
    if(message.requestId.startsWith('library-history-'))historyPending=false;
    if(message.requestId===editorRequest){
      editorRequest='';el<HTMLButtonElement>('librarySave').disabled=false;
      if(message.ok){
        selectedKey=editorKind==='history'?`history:${value('libraryText')}`:`${editorKind}:${editing}`;
        if(editorKind==='quotes')quoteSource='custom';
        closeEditor();
      }else editorError(message.message);
    }
    if (!message.ok) {
      if(message.requestId.startsWith('library-quoteCatalog-'))poetryRequested=false;
      status(message.message);
      renderStudy();
      return;
    }
    if (message.requestId.startsWith("library-review-")) {
      study.shift();
      reviewed++;
      renderStudy();
    }
    if(typeof message.exported==="string"){
      const url=URL.createObjectURL(new Blob([message.exported],{type:"application/json"}));
      const link=document.createElement("a");link.href=url;link.download=`知语-${value("libraryScope").trim()||"应用"}-词库.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
    }
    const oldHistory=history;
    book = (message.book ?? book) as unknown as Book;
    if(Array.isArray(message.quoteEntries))poetry=message.quoteEntries.map(x=>({...x,builtin:true,quoteSource:'poetry'}));
    if (Array.isArray(message.historyEntries)) history=message.historyEntries;
    else if(Array.isArray(message.history)) history=message.history.map(text=>({text,copies:1}));
    history.sort((a,b)=>b.copies-a.copies);
    if(message.requestId.startsWith('library-history-')&&oldHistory.length===history.length&&oldHistory.every((entry,i)=>entry.text===history[i].text&&entry.copies===history[i].copies))return;
    if (Array.isArray(message.study)) {
      study = message.study as typeof study;
      renderStudy();
    }
    // Routine successful reads, writes, copies and refreshes have no status row on any library page.
    // Polling cannot erase an unresolved editing error; failures have a separate dismissible alert.
    if(!message.requestId.startsWith('library-history-'))status('');
    render();
  });
  personalAction("status");
  renderStudy();
}
