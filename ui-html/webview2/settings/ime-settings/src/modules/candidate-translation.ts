import {updateConfig} from './config-sync';
import {setupModelFetch} from './model-fetch';

export type CandidateTranslationProvider = 'legacy'|'dictionary'|'tencent'|'custom'|'niutrans'|'ai';
let selection:CandidateTranslationProvider='legacy';
let aiProvider='deepseek';
let tokens:Record<string,string>={},endpoints:Record<string,string>={},models:Record<string,string>={};
let resetModels=()=>{};
const defaults:Record<string,[string,string]>={
  deepseek:['https://api.deepseek.com/chat/completions','deepseek-flash'],
  openai:['https://api.openai.com/v1/chat/completions','gpt-4o-mini'],
  siliconflow:['https://api.siliconflow.cn/v1/chat/completions','Qwen/Qwen3-8B'],
  groq:['https://api.groq.com/openai/v1/chat/completions','llama-3.3-70b-versatile'],
  custom:['',''],local:['http://127.0.0.1:11434/v1/chat/completions','']
};
let changed=()=>{};
const field=(id:string)=>document.getElementById(id) as HTMLInputElement|null;
export function candidateTranslationSelection():CandidateTranslationProvider{return selection;}
export function selectCandidateTranslation(provider:CandidateTranslationProvider):void{
  selection=provider;updateConfig('candidate_translation.provider',provider);changed();
}
export function candidateTranslationAiConfig():Record<string,string>{
  return {provider:aiProvider,endpoint:field('candidateAiEndpoint')?.value.trim()??'',
    model:field('candidateAiModel')?.value.trim()??'',token:field('candidateAiToken')?.value.trim()??''};
}
function applyFields():void{
  const provider=document.getElementById('candidateAiProvider') as HTMLSelectElement|null;
  if(provider)provider.value=aiProvider;
  const fallback=defaults[aiProvider]??['',''];
  for(const [id,value] of [['candidateAiEndpoint',endpoints[aiProvider]??fallback[0]],
    ['candidateAiModel',models[aiProvider]??fallback[1]],['candidateAiToken',tokens[aiProvider]??'']]){
    const input=field(id);if(input&&document.activeElement!==input)input.value=value;
  }
  const token=field('candidateAiToken');if(token)token.placeholder=aiProvider==='local'?'本地服务可留空':'填写当前提供商的 API Key';
}
export function applyCandidateTranslationConfig(config:any):void{
  selection=['legacy','dictionary','tencent','custom','niutrans','ai'].includes(config?.provider)?config.provider:'legacy';
  const previous=aiProvider;aiProvider=defaults[config?.ai_provider]?config.ai_provider:'deepseek';
  for(const [key,set] of [['tokens',(v:Record<string,string>)=>tokens=v],['endpoints',(v:Record<string,string>)=>endpoints=v],['models',(v:Record<string,string>)=>models=v]] as const){
    const map:Record<string,string>={};const raw=config?.[key];
    if(raw&&typeof raw==='object')Object.entries(raw).forEach(([id,v])=>{if(typeof v==='string')map[id]=v;});
    const effective=config?.[key==='tokens'?'token':key==='endpoints'?'endpoint':'model'];
    if(typeof effective==='string')map[aiProvider]=effective;set(map);
  }
  if(previous!==aiProvider)resetModels();applyFields();changed();
}
export function setupCandidateTranslation(onChanged:()=>void):void{
  changed=onChanged;
  const provider=document.getElementById('candidateAiProvider') as HTMLSelectElement|null;
  provider?.addEventListener('change',()=>{
    const config=candidateTranslationAiConfig();
    tokens[aiProvider]=config.token;endpoints[aiProvider]=config.endpoint;models[aiProvider]=config.model;
    for(const name of ['token','endpoint','model'])updateConfig(`candidate_translation.${name}_${aiProvider}`,config[name]);
    aiProvider=defaults[provider.value]?provider.value:'deepseek';
    updateConfig('candidate_translation.ai_provider',aiProvider);resetModels();applyFields();changed();
  });
  for(const [id,key,slots] of [['candidateAiEndpoint','endpoint',()=>endpoints],['candidateAiModel','model',()=>models],['candidateAiToken','token',()=>tokens]] as const){
    const input=field(id);input?.addEventListener('change',()=>{
      input.value=input.value.trim();slots()[aiProvider]=input.value;updateConfig(`candidate_translation.${key}`,input.value);resetModels();changed();
    });
  }
  resetModels=setupModelFetch({inputId:'candidateAiModel',arrowId:'candidateAiModelArrow',menuId:'candidateAiModelMenu',buttonId:'candidateAiModelFetch',statusId:'candidateAiModelStatus',service:()=> 'translation.ai',readConfig:candidateTranslationAiConfig});
}
