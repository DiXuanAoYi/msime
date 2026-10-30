import { setupLocalTts } from './local-tts';
import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';
import { updateConfig } from './config-sync';
import { fillVoices, LANGUAGE_NAMES, parseModelOptions, renderModelControls, type SpeechModel } from './speech-model-options';
import { applyVoiceStudioConfig, setupVoiceStudio } from './voice-studio';

type Provider = { name: string; note: string; links: [string, string][]; fields: [string, string, string?][] };
export const SPEECH_PROVIDERS: Record<string, Provider> = {
  sapi: { name: 'Windows 本机语音（离线）', note: '无需下载模型。自动按原词 / 译文语言选择已安装的语音包；不会联网。', links: [['语音包说明','https://support.microsoft.com/windows/appendix-a-supported-languages-and-voices-4486e345-7730-53da-fcfe-55cc64300f01']], fields: [['sapi_voice_zh','中文音色'],['sapi_voice_en','英文音色']] },
  local: { name: '本地模型 / Edge-TTS 桥接', note: '下方离线模型在本机合成；Edge-TTS 是可选在线服务，启用后文本发送到微软。也可连接本机兼容 /v1/audio/speech 服务，并填写自部署服务里已登记的参考音色 ID。音色克隆不会让模型获得原本不支持的方言。', links: [], fields: [['local_endpoint','本地服务地址'],['local_model','模型 ID'],['local_voice_zh','中文音色 / 参考音色 ID'],['local_voice_en','其他语言音色 / 参考音色 ID']] },
  azure: { name: 'Microsoft Azure Neural TTS', note: '支持 SSML。F0 免费层每月 50 万神经语音字符；付费价格依地域与层级变化，请查看官方报价。所选文字会发送到 Azure。', links: [['开通 / 控制台','https://portal.azure.com'],['API 文档','https://learn.microsoft.com/azure/ai-services/speech-service/rest-text-to-speech'],['价格 / 免费层','https://azure.microsoft.com/pricing/details/speech/']], fields: [['azure_key','Speech Key','password'],['azure_region','资源地域（如 eastasia）'],['azure_voice_zh','中文音色'],['azure_voice_en','英文音色']] },
  volcengine: { name: '火山引擎 / 豆包 TTS', note: 'HTTP 单向流式接口，支持多语种与丰富音色。资源 ID 与音色必须匹配已开通服务。所选文字会发送到火山引擎。', links: [['开通服务 / 获取凭据','https://console.volcengine.com/speech/app'],['API / 音色说明','https://www.volcengine.com/docs/6561/1598757']], fields: [['volcengine_app_id','App ID'],['volcengine_key','Access Token','password'],['volcengine_resource','Resource ID'],['volcengine_voice_zh','中文音色 ID'],['volcengine_voice_en','英文音色 ID']] },
  tencent: { name: '腾讯云 TTS', note: '使用 TextToVoice 与 TC3 签名。SecretId / SecretKey 和数字 VoiceType 需拥有语音合成权限。所选文字会发送到腾讯云。', links: [['开通 / 控制台','https://console.cloud.tencent.com/tts'],['API / 音色列表','https://cloud.tencent.com/document/product/1073/37995'],['价格','https://cloud.tencent.com/product/tts/pricing']], fields: [['tencent_secret_id','SecretId'],['tencent_secret_key','SecretKey','password'],['tencent_region','地域'],['tencent_voice_zh','中文 VoiceType'],['tencent_voice_en','英文 VoiceType']] },
  elevenlabs: { name: 'ElevenLabs', note: 'Eleven v3 注重情感表达，可用 [laughs]、[whispers] 等音频标签；Flash v2.5 支持 32 种语言、偏重低延迟。速度限制与费用依模型和套餐变化。所选文字会发送到 ElevenLabs。', links: [['获取 API Key','https://elevenlabs.io/app/settings/api-keys'],['模型 / 音色文档','https://elevenlabs.io/docs/overview/models'],['价格','https://elevenlabs.io/pricing/api']], fields: [['elevenlabs_key','API Key','password'],['elevenlabs_model','模型'],['elevenlabs_voice_zh','中文 Voice ID'],['elevenlabs_voice_en','英文 Voice ID']] }
};
let config: Record<string,string> = {};
let voices: {id:string;name:string}[] = [];
let modelInfo:SpeechModel = {};
const post = (action:'status'|'preview'|'stop', text?:string, language?:'zh'|'en') => window.chrome?.webview?.postMessage(serializeHostMessage({type:'speechRequest',data:{requestId:`speech-${Date.now()}`,action,...(text?{text,language}: {})}}));
function link(parent: HTMLElement, label:string, url:string): void {
  const a=document.createElement('a');a.textContent=label;a.href=url;
  a.addEventListener('click',e=>{e.preventDefault();window.chrome?.webview?.postMessage(serializeHostMessage({type:'openExternalUrl',data:url}));});parent.append(a);
}
function renderFields(): void {
  const root=document.getElementById('speechFields'),note=document.getElementById('speechProviderNote'),links=document.getElementById('speechLinks');
  if(!root||!note||!links)return;
  root.replaceChildren();links.replaceChildren();
  const provider=SPEECH_PROVIDERS[config.provider]??SPEECH_PROVIDERS.sapi;note.textContent=provider.note;
  for(const [label,url] of provider.links)link(links,label,url);
  for(const [key,label,type] of provider.fields){
    const row=document.createElement('label');row.className='speech-field';row.append(document.createTextNode(label));
    let input:HTMLInputElement|HTMLSelectElement;
    if(key.startsWith('local_voice') && config.provider==='local' && modelInfo.voices?.length){
      const select=document.createElement('select');input=select;fillVoices(select,modelInfo.voices,config[key]??'');
    }else if(key.startsWith('sapi_voice')||key==='elevenlabs_model'){
      const select=document.createElement('select');input=select;
      const options=key==='elevenlabs_model'?[{id:'eleven_flash_v2_5',name:'Flash v2.5 · 快速'},{id:'eleven_v3',name:'Eleven v3 · 情感'}]:[{id:'',name:'按语言自动选择'},...voices];
      for(const o of options)select.add(new Option(o.name,o.id));
      if(config[key]&&!options.some(o=>o.id===config[key]))select.add(new Option(config[key]+'（当前配置）',config[key]));
    }else{input=document.createElement('input');input.type=type??'text';input.autocomplete='off';input.spellcheck=false;
      if(key.includes('_voice_')&&modelInfo.voices?.length){const list=document.createElement('datalist');list.id=`speech-choices-${key}`;for(const voice of modelInfo.voices){const option=document.createElement('option');option.value=voice.id;option.label=voice.name;list.append(option);}input.setAttribute('list',list.id);row.append(list);}
    }
    input.value=config[key]??'';input.id=`speech-${key}`;
    input.addEventListener('change',()=>{config[key]=input.value.trim();updateConfig(`speech.${key}`,config[key]);});row.append(input);
    if(key.startsWith('local_voice') && input instanceof HTMLSelectElement){
      const custom=document.createElement('input');custom.type='text';custom.placeholder='自定义或已登记的参考音色 ID';custom.maxLength=300;custom.value=config[key]??'';
      custom.addEventListener('change',()=>{config[key]=custom.value.trim();updateConfig(`speech.${key}`,config[key]);fillVoices(input as HTMLSelectElement,modelInfo.voices??[],config[key]);});
      row.append(custom);
    }
    root.append(row);
  }
  const controls=document.getElementById('speechModelControls');
  if(controls){
    controls.hidden=config.provider!=='local';
    if(config.provider==='local'){
      const options=parseModelOptions(config.local_options);
      renderModelControls(controls,modelInfo,options[config.local_model]??{},value=>{
        const current=parseModelOptions(config.local_options);current[config.local_model]=value;
        config.local_options=JSON.stringify(current);updateConfig('speech.local_options',config.local_options);
      });
    }
  }
  const language=document.getElementById('speechLanguage') as HTMLSelectElement|null;
  if(language){const current=language.value;const choices=config.provider==='local'&&modelInfo.languages?.length?modelInfo.languages:['zh','en','ja','ko','fr','de','es','it','pt','ru'];language.replaceChildren();for(const id of choices)language.add(new Option(LANGUAGE_NAMES[id]??id,id));language.value=choices.includes(current)?current:choices[0];}
  applyVoiceStudioConfig(config,modelInfo,voices);
}
export function applySpeechConfig(value:Record<string,string>):void{
  config={...value};
  for(const key of ['provider','rate','volume']){const e=document.getElementById(`speech-${key}`) as HTMLInputElement|null;if(e)e.value=config[key]??'';}
  const enabled=document.getElementById('speech-enabled') as HTMLInputElement|null;if(enabled)enabled.checked=config.enabled!=='false';
  renderFields();
}
export function setupSpeech():void{
  const provider=document.getElementById('speech-provider') as HTMLSelectElement|null;if(!provider||provider.dataset.ready)return;provider.dataset.ready='1';
  for(const [id,p] of Object.entries(SPEECH_PROVIDERS))provider.add(new Option(p.name,id));
  const panelForDialects=document.getElementById('speechPanel');
  if(panelForDialects && !document.getElementById('speechDialectCoverage')){
    const details=document.createElement('details');details.id='speechDialectCoverage';
    const title=document.createElement('summary');title.textContent='方言和民族语言：查看本地 / 云端支持范围';details.append(title);
    const table=document.createElement('table');table.style.cssText='width:100%;text-align:left;border-collapse:collapse';
    for(const [name,support] of [
      ['北京话、四川话','Qwen 内置 Dylan / Eric；MOSS 有京味参考音色。其他模型不自动获得这些方言。'],
      ['上海话、河南话','Azure 音色与方言参数已接入，需服务凭据。本版未内置对应离线模型；火山需使用控制台明确支持此方言的音色，Vivi 不默认标为支持。'],
      ['粤语、闽南语、东北、陕西、山西、安徽、湖南、甘肃、山东、辽宁、天津等','按 Azure / 火山服务实际音色与资源权限选择；火山音色可填写“音色ID|方言代码”。Vivi 预置只列已核实的东北、陕西、四川。'],
      ['藏语','MMS 中部藏语、安多藏语，输入藏文；按需下载，官方单音色，非商业许可。'],
      ['维吾尔语、蒙古语、哈萨克语','MMS 原生模型；维吾尔语输入阿拉伯文字，蒙古语输入西里尔文字。按需下载，官方单音色，非商业许可。'],
      ['苏州话','暂无已核实内置模型或音色。可连接自己已验证的本机兼容服务和参考音色；上海话不会标成苏州话。'],
    ]){const tr=document.createElement('tr');for(const text of [name,support]){const td=document.createElement('td');td.textContent=text;td.style.padding='8px';tr.append(td);}table.append(tr);}details.append(table);
    const note=document.createElement('p');note.textContent='新增模型权重不会自动下载；未下载模型和云音色须先试听确认。参考音色 ID 由你的兼容服务登记，本输入法不会自行采集或克隆他人声音。';details.append(note);
    panelForDialects.append(details);
  }
  provider.addEventListener('change',()=>{config.provider=provider.value;updateConfig('speech.provider',provider.value);renderFields();});
  document.getElementById('speech-enabled')?.addEventListener('change',e=>updateConfig('speech.enabled',String((e.target as HTMLInputElement).checked)));
  for(const key of ['rate','volume'])document.getElementById(`speech-${key}`)?.addEventListener('change',e=>updateConfig(`speech.${key}`,(e.target as HTMLInputElement).value));
  document.getElementById('speechPreview')?.addEventListener('click',()=>{const text=(document.getElementById('speechText') as HTMLTextAreaElement).value.trim();const lang=(document.getElementById('speechLanguage') as HTMLSelectElement).value as 'zh'|'en';if(text)post('preview',text,lang);});
  document.getElementById('speechStop')?.addEventListener('click',()=>post('stop'));
  onHostMessage('speechResponse',message=>{if(message.type!=='speechResponse')return;const el=document.getElementById('speechStatus');if(el)el.textContent=message.message;
    const next=message.voices as {id:string;name:string}[];const nextModel=(message.modelInfo??{}) as SpeechModel;
    if(JSON.stringify(next)!==JSON.stringify(voices)||JSON.stringify(nextModel)!==JSON.stringify(modelInfo)){voices=next;modelInfo=nextModel;renderFields();}});
  const panel=document.getElementById('speechPanel') as HTMLDetailsElement;let timer=0;
  const updatePolling=()=>{clearInterval(timer);if(panel.open){post('status');timer=window.setInterval(()=>{if(!document.hidden)post('status');},2000);}};
  panel.addEventListener('toggle',updatePolling);
  window.addEventListener('beforeunload',()=>clearInterval(timer));
  setupVoiceStudio();applySpeechConfig(config);updatePolling();setupLocalTts();
}
