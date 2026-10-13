import { serializeHostMessage } from "../../../../shared/messages";
import { onHostMessage } from "../utils/host-messages";
import "../styles/modules/prompt-library.css";
import "../styles/modules/workbench-layout.css";
import "../styles/modules/clipboard.css";
import {confirmDialog} from '../utils/confirm-dialog';
import {updateConfig} from './config-sync';
type Action =
  | "status"
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
): void {
  window.chrome?.webview?.postMessage(
    serializeHostMessage({
      type: "personalLibraryRequest",
      data: {
        requestId: `library-${action}-${Date.now()}-${Math.random()}`,
        action,
        ...extra,
      },
    }),
  );
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
};
export function libraryPage(entries: Entry[], query: string, page: number, pageSize = 12): {items:Entry[];total:number;page:number;pages:number} {
  const needle=query.trim().toLocaleLowerCase();
  const filtered=needle?entries.filter(x=>[x.name,x.text,x.application,x.code].some(value=>value?.toLocaleLowerCase().includes(needle))):entries;
  const size=Number.isInteger(pageSize)&&pageSize>0?Math.min(pageSize,100):12;
  const pages=Math.max(1,Math.ceil(filtered.length/size));
  const current=Math.max(0,Math.min(Number.isFinite(page)?Math.floor(page):0,pages-1));
  return {items:filtered.slice(current*size,(current+1)*size),total:filtered.length,page:current,pages};
}
type Book = {
  groups: { id: string; name: string }[];
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
  let book: Book = { groups: [], clips: [], rules: [], favorites: [], applications:[],appLearning:[] },
    history: string[] = [],
    editing = "", page = 0, selectedKey = "";
  let study: { word: string; language: string; gloss: string }[] = [],
    reviewed = 0;
  const status = (s: string) => {
    el("libraryStatus").textContent = s;
  };
  const button = (name: string, action: () => void) => {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = name;
    b.addEventListener("click", action);
    return b;
  };
  const reset = () => {
    editing = "";
    for (const id of ["libraryName", "libraryText", "libraryCode"])
      el<HTMLInputElement>(id).value = "";
    el<HTMLInputElement>("libraryEnabled").checked = true;
  };
  const render = () => {
    const view = value("libraryView"),
      isClip = view === "clips",
      isRule = view === "rules";
    root.querySelectorAll<HTMLButtonElement>('[data-library-view]').forEach(tab=>tab.setAttribute('aria-pressed',String(tab.dataset.libraryView===view)));
    el('libraryBrowse').hidden=view==='study';
    el('libraryCaptureEnabled').parentElement!.hidden=view!=='history';
    el('libraryClearHistory').hidden=view!=='history';
    el<HTMLButtonElement>('libraryClearHistory').disabled=!history.length;
    el("libraryGroups").hidden = !isClip;
    el("libraryApplicationManager").hidden = !isRule && view !== "learning";
    const appSelect=el<HTMLSelectElement>("libraryApplicationSelect");
    const app=value("libraryScope").trim().toLowerCase();
    appSelect.replaceChildren(new Option("选择或填写新软件", ""),...(book.applications??[]).map(x=>new Option(x.application+(x.enabled?" · 已启用":" · 已关闭"),x.application)));
    appSelect.value=app;
    const profile=(book.applications??[]).find(x=>x.application===app);
    if(profile){el<HTMLInputElement>("libraryIsolation").checked=profile.enabled;el<HTMLInputElement>("libraryAutoLearn").checked=profile.learn;el<HTMLInputElement>("libraryFrequency").checked=profile.frequency;}
    el("libraryEditor").hidden = !isClip && !isRule;
    el("libraryAppFields").hidden = !isRule;
    el("libraryStudy").hidden = view !== "study";
    el("librarySearchArea").hidden = view === "study";
    el("libraryPager").hidden = view === "study";
    el("libraryNew").hidden = !isClip && !isRule;
    el("libraryNew").textContent=isRule?'新建软件词条':'新建常用语';
    const group = el<HTMLSelectElement>("libraryGroup");
    const old = group.value;
    group.replaceChildren(...book.groups.map((g) => new Option(g.name, g.id)));
    group.value = book.groups.some((g) => g.id === old)
      ? old
      : (book.groups[0]?.id ?? "");
    const rows = el("libraryRows");
    rows.replaceChildren();
    if (view === "study") return;
    const entries: Entry[] =
      view === "history"
        ? history.map((text, i) => ({ id: String(i), name: "最近复制", text }))
        : view === "clips"
          ? book.clips.filter((x) => x.group === group.value)
          : view === "rules"
            ? book.rules.filter(x=>!app||x.application===app)
            : view === "learning"
              ? (book.appLearning??[]).filter(x=>!app||x.application===app).map(x=>({...x,name:`${x.language??"zh"} · ${x.frequency??0} 次${x.deleted?' · 本软件已隐藏':''}${x.pinned?' · 本软件置顶':''}`}))
              : book.favorites;
    const visible=libraryPage(entries,value("librarySearch"),page);page=visible.page;
    el("libraryResultCount").textContent=`共 ${visible.total} 条`;
    el("libraryPage").textContent=`${page+1} / ${visible.pages}`;
    el<HTMLButtonElement>("libraryPrevious").disabled=page===0;
    el<HTMLButtonElement>("libraryNext").disabled=page+1>=visible.pages;
    if (!visible.items.length) {
      const p = document.createElement("p");
      p.textContent =
        view === "favorites"
          ? "在语音工作台生成后点击“收藏语音”，即可在此重复播放或复制。"
          : "暂无内容。";
      rows.append(p);
    }
    const entryKey=(x:Entry)=>`${view}:${view==='history'?x.text:x.id}`;
    if(!visible.items.some(x=>entryKey(x)===selectedKey))selectedKey=visible.items[0]?entryKey(visible.items[0]):'';
    const renderPreview=(x:Entry|undefined)=>{
      const title=el('libraryPreviewTitle'),text=el<HTMLTextAreaElement>('libraryPreviewText'),actions=el('libraryPreviewActions');
      actions.replaceChildren();
      if(!x){title.textContent='暂无可预览的内容';text.value='';el('libraryPreviewMeta').textContent='';return;}
      title.textContent=view==='history'?'复制记录':x.name;
      text.value=x.text??'';
      el('libraryPreviewMeta').textContent=[x.application,x.code,`${Array.from(x.text??'').length} 字符`,x.enabled===false?'已停用':''].filter(Boolean).join(' · ');
      if (view === "favorites") {
        actions.append(
          button("播放", () => personalAction("playFavorite", { id: x.id })),
          button("复制音频文件", () =>
            personalAction("copyFavorite", { id: x.id }),
          ),
        );
      } else
        actions.append(
          button("复制", () => personalAction("copy", { text: x.text ?? "" })),
        );
      if (view === "clips" || view === "rules")
        actions.append(
          button("编辑", () => {
            editing = x.id;
            el<HTMLInputElement>("libraryName").value = x.name;
            el<HTMLTextAreaElement>("libraryText").value = x.text ?? "";
            if (view === "rules") {
              el<HTMLInputElement>("libraryApplication").value =
                x.application ?? "";
              el<HTMLInputElement>("libraryCode").value = x.code ?? "";
              el<HTMLInputElement>("libraryEnabled").checked =
                x.enabled !== false;
            }
            el<HTMLDetailsElement>("libraryEditor").open=true;
            el("libraryEditor").scrollIntoView({ block: "start" });
          }),
        );
      if (view === "history")
        actions.append(
          button("加入常用语", () =>
            personalAction("putClip", {
              name: (x.text ?? "").slice(0, 30) || "收藏文本",
              text: x.text,
              group:
                book.groups.find((g) => g.id === "phrases")?.id ??
                book.groups[0]?.id,
            }),
          ),
        );
      if (view !== "history")
        actions.append(
          button("删除", () =>
            view === "learning"
              ? personalAction("removeLearning", {application:x.application,id:x.id})
              : personalAction("remove", { section: view, id: x.id }),
          ),
        );
      if(view==='history')actions.append(button('删除此记录',async()=>{
        if(await confirmDialog('删除这条复制记录？保存的常用语和其他资料不会删除。'))personalAction('removeHistory',{text:x.text});
      }));
    };
    for(const x of visible.items){
      const row=document.createElement('button');row.type='button';row.className='clipboard-item';row.setAttribute('role','option');
      const key=entryKey(x),selected=key===selectedKey;row.setAttribute('aria-selected',String(selected));
      const title=document.createElement('strong'),snippet=document.createElement('span'),meta=document.createElement('small');
      title.textContent=view==='history'?(x.text??'').split(/\r?\n/)[0].slice(0,72)||'空白文本':x.name;
      snippet.textContent=(x.text??'').replace(/\s+/g,' ').slice(0,150);
      meta.textContent=[x.application,x.code,`${Array.from(x.text??'').length} 字符`].filter(Boolean).join(' · ');
      row.append(title,snippet,meta);
      row.onclick=()=>{selectedKey=key;rows.querySelectorAll<HTMLButtonElement>('.clipboard-item').forEach(b=>b.setAttribute('aria-selected',String(b===row)));renderPreview(x);};
      row.ondblclick=()=>{if(x.text)personalAction('copy',{text:x.text});};
      row.onkeydown=e=>{if(e.key!=='ArrowDown'&&e.key!=='ArrowUp')return;e.preventDefault();const buttons=Array.from(rows.querySelectorAll<HTMLButtonElement>('.clipboard-item'));const index=buttons.indexOf(row)+(e.key==='ArrowDown'?1:-1);const next=buttons[index];if(next){next.click();next.focus();}};
      rows.append(row);
    }
    renderPreview(visible.items.find(x=>entryKey(x)===selectedKey));
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
    page=0;reset();render();
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
    el<HTMLSelectElement>('libraryView').value=tab.dataset.libraryView??'history';el('libraryView').dispatchEvent(new Event('change'));
  });
  el('libraryClearHistory').onclick=async()=>{
    if(await confirmDialog('清空全部复制记录？常用语、分组、语音收藏和词库不受影响。',{title:'清空复制记录',confirmText:'清空'}))personalAction('clearHistory');
  };
  el<HTMLInputElement>('libraryCaptureEnabled').onchange=()=>updateConfig('utility.clipboard_history',el<HTMLInputElement>('libraryCaptureEnabled').checked);
  onHostMessage('configSnapshot',r=>{const utility=r.data.utility as Record<string,unknown>|undefined;const enabled=utility?.clipboard_history;if(typeof enabled==='boolean')el<HTMLInputElement>('libraryCaptureEnabled').checked=enabled;});
  window.chrome?.webview?.postMessage(serializeHostMessage({type:'configRequest'}));
  el("libraryRefresh").onclick = () => personalAction("status");
  el("libraryView").onchange = () => {
    page=0;el<HTMLInputElement>("librarySearch").value="";
    reset();
    render();
  };
  el("libraryGroup").onchange = () => {
    page=0;
    reset();
    render();
  };
  el("libraryNew").onclick = ()=>{reset();el<HTMLDetailsElement>("libraryEditor").open=true;el("libraryEditor").scrollIntoView({block:"start"});};
  el("librarySearch").oninput=()=>{page=0;render();};
  el("libraryPrevious").onclick=()=>{page--;render();};
  el("libraryNext").onclick=()=>{page++;render();};
  el("libraryAddGroup").onclick = () =>
    personalAction("putGroup", { name: value("libraryGroupName").trim() });
  el("libraryRenameGroup").onclick = () =>
    personalAction("putGroup", {
      id: value("libraryGroup"),
      name: value("libraryGroupName").trim(),
    });
  el("libraryRemoveGroup").onclick = () =>
    personalAction("removeGroup", { id: value("libraryGroup") });
  el("librarySave").onclick = () => {
    const rule = value("libraryView") === "rules";
    // Keep the identity across repeated saves, including a failed validation retry.
    if (!editing) editing = crypto.randomUUID();
    personalAction(rule ? "putRule" : "putClip", {
      id: editing,
      name: value("libraryName").trim(),
      text: value("libraryText"),
      ...(rule
        ? {
            application: value("libraryApplication").trim(),
            code: value("libraryCode").trim(),
            enabled: el<HTMLInputElement>("libraryEnabled").checked,
          }
        : { group: value("libraryGroup") }),
    });
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
    if (!message.ok) {
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
      const link=document.createElement("a");link.href=url;link.download=`水杉-${value("libraryScope").trim()||"应用"}-词库.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
    }
    book = (message.book ?? book) as unknown as Book;
    history = message.history ?? history;
    if (Array.isArray(message.study)) {
      study = message.study as typeof study;
      renderStudy();
    }
    status(message.message || (message.requestId.startsWith("library-status-")?"已读取本机记录":"已保存到本机"));
    render();
  });
  personalAction("status");
  renderStudy();
}
