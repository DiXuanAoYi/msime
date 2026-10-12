import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

type ModelService = 'ai.assistant' | 'voice.polish' | 'translation.ai';
type Options = {inputId:string; arrowId:string; menuId:string; buttonId:string; statusId:string;
  service:()=>ModelService; readConfig:()=>Record<string,string>};
let sequence=0;
export function setupModelFetch(o:Options):()=>void {
  const input=document.getElementById(o.inputId) as HTMLInputElement|null;
  const menu=document.getElementById(o.menuId),arrow=document.getElementById(o.arrowId);
  const button=document.getElementById(o.buttonId) as HTMLButtonElement|null;
  const status=document.getElementById(o.statusId);
  if(!input||!menu||!arrow||!button)return ()=>{};
  input.setAttribute('role','combobox');input.setAttribute('aria-controls',o.menuId);
  input.setAttribute('aria-autocomplete','list');input.setAttribute('aria-expanded','false');
  menu.setAttribute('role','listbox');
  let pending='',models:string[]=[],active=-1,timer:ReturnType<typeof setTimeout>|undefined;
  const close=()=>{menu.classList.remove('open');input.setAttribute('aria-expanded','false');input.removeAttribute('aria-activedescendant');active=-1;};
  const visible=()=>Array.from(menu.querySelectorAll<HTMLButtonElement>('button:not([hidden])'));
  const highlight=()=>{visible().forEach((row,i)=>{row.classList.toggle('active',i===active);row.setAttribute('aria-selected',String(i===active));});
    const row=visible()[active];if(row){input.setAttribute('aria-activedescendant',row.id);row.scrollIntoView?.({block:'nearest'});}};
  const choose=(model:string)=>{input.value=model;input.dispatchEvent(new Event('change',{bubbles:true}));close();input.focus();};
  const render=()=>{menu.replaceChildren();active=-1;models.forEach((model,i)=>{const row=document.createElement('button');
    row.type='button';row.className='model-option';row.id=`${o.menuId}-${i}`;row.setAttribute('role','option');row.textContent=model;row.title=model;
    row.hidden=!!input.value&&!model.toLowerCase().includes(input.value.toLowerCase());row.addEventListener('mousedown',e=>e.preventDefault());
    row.addEventListener('click',()=>choose(model));menu.append(row);});};
  const open=()=>{menu.classList.add('open');input.setAttribute('aria-expanded','true');};
  const settle=()=>{pending='';clearTimeout(timer);button.disabled=false;button.textContent='查询模型';};
  const fetch=()=>{if(pending||!window.chrome?.webview)return;pending=`models-${Date.now()}-${++sequence}`;button.disabled=true;button.textContent='查询中…';
    if(status)status.textContent='只查询模型名称，不下载权重';
    window.chrome.webview.postMessage(serializeHostMessage({type:'apiModelList',data:{requestId:pending,service:o.service(),config:o.readConfig()}}));
    timer=setTimeout(()=>{settle();if(status)status.textContent='查询超时，仍可手动填写模型';},20000);};
  button.addEventListener('click',fetch);
  arrow.addEventListener('click',()=>{if(menu.classList.contains('open'))close();else if(models.length){render();menu.querySelectorAll<HTMLButtonElement>('button').forEach(row=>row.hidden=false);open();}else fetch();});
  input.addEventListener('input',()=>{if(models.length){render();open();}});
  input.addEventListener('keydown',e=>{if(e.key==='Escape'){close();return;}if(e.key==='ArrowDown'||e.key==='ArrowUp'){
    e.preventDefault();if(!models.length){fetch();return;}if(!menu.classList.contains('open')){render();open();}
    const rows=visible();if(rows.length){active=active<0?(e.key==='ArrowDown'?0:rows.length-1):(active+(e.key==='ArrowDown'?1:-1)+rows.length)%rows.length;highlight();}
  }else if(e.key==='Enter'&&menu.classList.contains('open')&&active>=0){e.preventDefault();visible()[active]?.click();}});
  document.addEventListener('pointerdown',e=>{if(!menu.parentElement?.contains(e.target as Node))close();});
  onHostMessage('apiModelListResult',r=>{if(!pending||r.requestId!==pending)return;settle();if(status)status.textContent=r.message;
    if(!r.ok)return;models=Array.from(new Set(r.models));render();menu.querySelectorAll<HTMLButtonElement>('button').forEach(row=>row.hidden=false);open();});
  return ()=>{settle();models=[];menu.replaceChildren();close();if(status)status.textContent='';};
}
