import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';
import { personalAction } from './personal-library';
import { fillVoices, LANGUAGE_NAMES, parseModelOptions, renderModelControls, type SpeechModel, type VoiceChoice } from './speech-model-options';

type Action='status'|'generate'|'stop'|'recordStart'|'recordStop'|'import'|'transcribe'|'play'|'route'|'copy'|'save';
let config:Record<string,string>={}, model:SpeechModel={}, sapiVoices:VoiceChoice[]=[];
let parameters:Record<string,unknown>={}, signature='', artifact='', dirty=true, submitted='', transcript='';
let phase='idle';
const field=<T extends HTMLElement>(id:string)=>document.getElementById(id) as T|null;
const post=(action:Action, extra:Record<string,unknown>={})=>window.chrome?.webview?.postMessage(serializeHostMessage({type:'voiceStudioRequest',data:{requestId:`studio-${Date.now()}`,action,...extra}}));
export async function openVoiceStudio(text:string):Promise<void>{
  const {loadContent}=await import('./sidebar');
  await loadContent('voice');
  document.querySelector<HTMLElement>('.sidebar .item[data-target="voice"]')?.click();
  const {showOnlyCurrentModule}=await import('../utils/common-utils');
  showOnlyCurrentModule('voice');
  const panel=field<HTMLDetailsElement>('voiceStudioPanel');
  const input=field<HTMLTextAreaElement>('studioText');
  if(!panel||!input)return;
  panel.open=true;input.value=text;dirty=true;updateButtons();
  panel.scrollIntoView({block:'start'});input.focus();
}
function fingerprint():string{return JSON.stringify([field<HTMLTextAreaElement>('studioText')?.value,field<HTMLSelectElement>('studioLanguage')?.value,field<HTMLSelectElement>('studioVoice')?.value,field<HTMLInputElement>('studioRate')?.value,parameters]);}
function updateButtons():void{
  const busy=['working','recording','routing'].includes(phase);
  for(const id of ['studioPlay','studioCopy','studioSave','studioRoute','studioFavorite']){const button=field<HTMLButtonElement>(id);if(button)button.disabled=!artifact||dirty||busy||(id==='studioRoute'&&!field<HTMLSelectElement>('studioDevice')?.value);}
  for(const id of ['studioGenerate','studioImport','studioRecord']){const button=field<HTMLButtonElement>(id);if(button)button.disabled=busy;}
  const end=field<HTMLButtonElement>('studioRecordStop');if(end)end.disabled=phase!=='recording';
  const count=field('studioBytes');if(count)count.textContent=`${new TextEncoder().encode(field<HTMLTextAreaElement>('studioText')?.value??'').length} / 2000 字节 · 最长音频 90 秒`;
}
function refreshDialect():void{
  const voice=field<HTMLSelectElement>('studioVoice')?.value;
  const selected=model.voices?.find(v=>v.id===voice);
  const label=field('studioDialect');if(label)label.textContent=selected?.dialect?`所选音色：${selected.dialect}。识别文字可先修改；不会自动改写语义。`:(model.dialectNote??'此服务没有已验证的方言音色。请先试听，确认后再手动发送。');
}
export function applyVoiceStudioConfig(value:Record<string,string>, info:SpeechModel, voices:VoiceChoice[]):void{
  config=value;model=info;sapiVoices=voices;
  const next=JSON.stringify([value.provider,value.local_model,info.voices,voices]);
  if(signature===next)return;signature=next;
  const language=field<HTMLSelectElement>('studioLanguage'),voice=field<HTMLSelectElement>('studioVoice');
  if(!language||!voice)return;
  const current=language.value||'zh';
  const languages=value.provider==='local'&&info.languages?.length?info.languages:['zh','en','ja','ko','fr','de','es','it','pt','ru'];
  language.replaceChildren();for(const id of languages)language.add(new Option(LANGUAGE_NAMES[id]??id,id));language.value=languages.includes(current)?current:languages[0];
  const choices=value.provider==='sapi'?sapiVoices:(info.voices??[]);
  const selected=value[`${value.provider}_voice_${language.value==='zh'?'zh':'en'}`]??'';
  fillVoices(voice,choices,selected);
  parameters={...(parseModelOptions(value.local_options)[value.local_model]??{})};
  const controls=field('studioOptions');if(controls){controls.hidden=value.provider!=='local';renderModelControls(controls,info,parameters,p=>{parameters=p;dirty=true;updateButtons();});}
  const provider=field('studioProvider');if(provider)provider.textContent=value.provider==='local'?`当前模型：${info.name??value.local_model}${value.local_model==='edge-tts'?' · 在线，会将生成文字发送到微软':' · 本机生成'}`:value.provider==='sapi'?'当前引擎：Windows 本机语音 · 离线':`当前云服务：${value.provider} · 点击生成将发送所填文字；音色使用上方设置。`;
  const rate=field<HTMLInputElement>('studioRate');if(rate)rate.value=value.rate??'1';
  dirty=true;refreshDialect();updateButtons();
}
export function setupVoiceStudio():void{
  const panel=field<HTMLDetailsElement>('voiceStudioPanel');if(!panel||panel.dataset.ready)return;panel.dataset.ready='1';
  for(const id of ['studioText','studioLanguage','studioVoice','studioRate'])field(id)?.addEventListener('input',()=>{dirty=true;refreshDialect();updateButtons();});
  field('studioDevice')?.addEventListener('change',updateButtons);
  field('studioFavorite')?.addEventListener('click',()=>{if(!artifact||dirty)return;personalAction('favorite',{artifactId:artifact,name:(field<HTMLTextAreaElement>('studioText')?.value??'收藏语音').slice(0,60)});});
  onHostMessage('personalLibraryResponse',message=>{if(message.requestId.startsWith('library-favorite-')&&field('studioStatus'))field('studioStatus')!.textContent=message.ok?'已收藏，可在“剪贴板与资料库”的语音收藏中播放、复制。':message.message;});
  field('studioGenerate')?.addEventListener('click',()=>{
    const text=field<HTMLTextAreaElement>('studioText')!.value.trim();const bytes=new TextEncoder().encode(text).length;
    if(!bytes||bytes>2000){field('studioStatus')!.textContent='请输入 1–2000 UTF-8 字节的文字，过长内容请分段。';return;}
    const rate=Number(field<HTMLInputElement>('studioRate')!.value);
    if(!Number.isFinite(rate)||rate<0.5||rate>2||panel.querySelector(':invalid')){field('studioStatus')!.textContent='请修正超出范围的语速或模型参数。';return;}
    submitted=fingerprint();dirty=true;phase='working';updateButtons();
    post('generate',{text,language:field<HTMLSelectElement>('studioLanguage')!.value,voice:field<HTMLSelectElement>('studioVoice')!.value,rate:Number(field<HTMLInputElement>('studioRate')!.value),optionsJson:JSON.stringify(parameters)});
  });
  for(const [id,action] of [['studioRecord','recordStart'],['studioRecordStop','recordStop'],['studioImport','import'],['studioTranscribe','transcribe'],['studioStop','stop']] as const)field(id)?.addEventListener('click',()=>post(action));
  for(const [id,action] of [['studioPlay','play'],['studioCopy','copy'],['studioSave','save'],['studioRoute','route']] as const)field(id)?.addEventListener('click',()=>{if(!artifact||dirty||['working','recording','routing'].includes(phase))return;post(action,{artifactId:artifact,...(action==='route'?{deviceId:Number(field<HTMLSelectElement>('studioDevice')!.value)}:{})});});
  field('studioCableHelp')?.addEventListener('click',()=>window.chrome?.webview?.postMessage(serializeHostMessage({type:'openExternalUrl',data:'https://vb-audio.com/Cable/'})));
  onHostMessage('voiceStudioResponse',message=>{
    const state=message.state as Record<string,unknown>;phase=String(state.phase??'idle');
    field('studioStatus')!.textContent=message.message||String(state.message??'');
    if(!message.ok)phase='error';
    const next=String(state.artifactId??'');
    if(next!==artifact){artifact=next;dirty=!next||fingerprint()!==submitted;}
    const recognized=String(state.transcript??'');
    if(recognized && recognized!==transcript){transcript=recognized;field<HTMLTextAreaElement>('studioText')!.value=recognized;dirty=true;}
    const devices=state.devices as {id:number;name:string;virtual:boolean}[];
    const select=field<HTMLSelectElement>('studioDevice')!;const deviceSignature=JSON.stringify(devices);
    if(select.dataset.signature!==deviceSignature){select.dataset.signature=deviceSignature;const current=select.value;select.replaceChildren();select.add(new Option('请选择虚拟音频输出设备',''));for(const d of devices??[])if(d.virtual)select.add(new Option(d.name,String(d.id)));select.value=[...select.options].some(o=>o.value===current)?current:'';}
    field('studioDuration')!.textContent=`输入音频：${Number(state.inputSeconds??0).toFixed(1)} 秒 · 已生成：${Number(state.outputSeconds??0).toFixed(1)} 秒`;
    const transcribe=field<HTMLButtonElement>('studioTranscribe')!;transcribe.disabled=!state.hasInput||['working','recording','routing'].includes(phase);
    updateButtons();
  });
  let timer=0;const poll=()=>{clearInterval(timer);if(panel.open){post('status');timer=window.setInterval(()=>post('status'),1000);}};
  panel.addEventListener('toggle',poll);window.addEventListener('beforeunload',()=>{clearInterval(timer);post('stop');});
  poll();applyVoiceStudioConfig(config,model,sapiVoices);updateButtons();
}
