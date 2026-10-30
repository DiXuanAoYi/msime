import {updateConfig} from './config-sync';
import {applyToggleState,setupToggleButton} from './shared';
export type EnglishRule={application:string;mode:'inherit'|'enabled'|'disabled'};
export function parseEnglishRules(raw:unknown):EnglishRule[]{
 try{const rows=typeof raw==='string'?JSON.parse(raw):raw;if(!Array.isArray(rows))return [];
 const seen=new Set<string>();return rows.filter(x=>x&&typeof x.application==='string'&&['inherit','enabled','disabled'].includes(x.mode)).map(x=>({...x,application:x.application.trim().toLowerCase()})).filter(x=>x.application&&!seen.has(x.application)&&seen.add(x.application)).slice(0,256);
 }catch{return [];}
}
let rules:EnglishRule[]=[],dirty=false;
const el=(id:string)=>document.getElementById(id);
function ruleApplicationError(application:string,index=-1):string {
 if(application.includes('\\')||!/^[^\/:*?"<>|\s]+\.exe$/i.test(application))return '请填写软件进程名，例如 code.exe';
 if(rules.some((rule,i)=>i!==index&&rule.application===application))return '已有该软件的规则';
 return '';
}
function renderRules():void {
 const area=el('englishRules');if(!area)return;area.replaceChildren();
 rules.forEach((rule,i)=>{const row=document.createElement('div');row.className='input-app-rule';const app=document.createElement('input');app.value=rule.application;app.setAttribute('aria-label','软件进程名');
 const mode=document.createElement('select');[['inherit','跟随全局'],['enabled','启用英文候选'],['disabled','关闭英文候选']].forEach(([v,n])=>mode.add(new Option(n,v)));mode.value=rule.mode;
 const save=()=>{const application=app.value.trim().toLowerCase();const error=ruleApplicationError(application,i);const status=el('englishRuleStatus');
  if(error){app.value=rule.application;mode.value=rule.mode;if(status)status.textContent=`${error}；已保留原规则，未保存无效修改`;return;}
  rule.application=application;app.value=application;rule.mode=mode.value as EnglishRule['mode'];dirty=true;updateConfig('input.english_candidate_app_rules',JSON.stringify(rules));if(status)status.textContent='已提交保存';};app.addEventListener('change',save);mode.addEventListener('change',save);
 const remove=document.createElement('button');remove.type='button';remove.textContent='删除';remove.addEventListener('click',()=>{rules.splice(i,1);dirty=true;updateConfig('input.english_candidate_app_rules',JSON.stringify(rules));renderRules();});row.append(app,mode,remove);area.append(row);});
}
export function setupInputAdvanced():void {
 setupToggleButton('englishCandidatesToggle',v=>updateConfig('input.english_candidates',v));
 setupToggleButton('wubiFourAutoToggle',v=>updateConfig('input.wubi_four_code_auto_commit',v));
 setupToggleButton('wubiFifthTopToggle',v=>updateConfig('input.wubi_fifth_code_top_commit',v));
 el('englishRuleAdd')?.addEventListener('click',()=>{const input=el('englishRuleApp') as HTMLInputElement|null;const app=input?.value.trim().toLowerCase()??'';
 const status=el('englishRuleStatus');const error=ruleApplicationError(app);if(error){if(status)status.textContent=error;return;}if(rules.length>=256){if(status)status.textContent='最多保存 256 条软件规则';return;}
 rules.push({application:app,mode:'inherit'});dirty=true;updateConfig('input.english_candidate_app_rules',JSON.stringify(rules));if(input)input.value='';if(status)status.textContent='已添加并自动保存';renderRules();});
 const vim=el('vimModeApps') as HTMLTextAreaElement|null;vim?.addEventListener('change',()=>{const apps=Array.from(new Set(vim.value.split(/[，,;；\n]+/).map(x=>x.trim().toLowerCase()).filter(Boolean)));updateConfig('input.vim_mode_apps',JSON.stringify(apps));});
}
export function applyInputAdvanced(c:Record<string,unknown>):void {
 if(typeof c.english_candidates==='boolean')applyToggleState('englishCandidatesToggle',c.english_candidates);
 if(typeof c.wubi_four_code_auto_commit==='boolean')applyToggleState('wubiFourAutoToggle',c.wubi_four_code_auto_commit);
 if(typeof c.wubi_fifth_code_top_commit==='boolean')applyToggleState('wubiFifthTopToggle',c.wubi_fifth_code_top_commit);
 const incoming=parseEnglishRules(c.english_candidate_app_rules);
 if(!dirty||JSON.stringify(incoming)===JSON.stringify(rules)){rules=incoming;dirty=false;renderRules();}
 const vim=el('vimModeApps') as HTMLTextAreaElement|null;if(vim&&document.activeElement!==vim&&typeof c.vim_mode_apps==='string')try{vim.value=JSON.parse(c.vim_mode_apps).join('\n');}catch{}
}
