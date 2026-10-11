import { setupLocalTts } from './local-tts';
import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';
import { updateConfig } from './config-sync';

type Provider = { name: string; note: string; links: [string, string][]; fields: [string, string, string?][] };
export const SPEECH_PROVIDERS: Record<string, Provider> = {
  sapi: { name: 'Windows 本机语音（离线）', note: '无需下载模型。自动按原词 / 译文语言选择已安装的语音包；不会联网。', links: [['语音包说明','https://support.microsoft.com/windows/appendix-a-supported-languages-and-voices-4486e345-7730-53da-fcfe-55cc64300f01']], fields: [['sapi_voice_zh','中文音色'],['sapi_voice_en','英文音色']] },
  local: { name: '本地模型 / Edge-TTS 桥接', note: '下方离线模型在本机合成；Edge-TTS 是可选在线服务，启用后文本发送到微软。也可连接本机兼容 /v1/audio/speech 服务。', links: [], fields: [['local_endpoint','本地服务地址'],['local_model','模型 ID'],['local_voice_zh','中文音色 ID'],['local_voice_en','英文音色 ID']] },
  azure: { name: 'Microsoft Azure Neural TTS', note: '支持 SSML。F0 免费层每月 50 万神经语音字符；付费价格依地域与层级变化，请查看官方报价。所选文字会发送到 Azure。', links: [['开通 / 控制台','https://portal.azure.com'],['API 文档','https://learn.microsoft.com/azure/ai-services/speech-service/rest-text-to-speech'],['价格 / 免费层','https://azure.microsoft.com/pricing/details/speech/']], fields: [['azure_key','Speech Key','password'],['azure_region','资源地域（如 eastasia）'],['azure_voice_zh','中文音色'],['azure_voice_en','英文音色']] },
  volcengine: { name: '火山引擎 / 豆包 TTS', note: 'HTTP 单向流式接口，支持多语种与丰富音色。资源 ID 与音色必须匹配已开通服务。所选文字会发送到火山引擎。', links: [['开通服务 / 获取凭据','https://console.volcengine.com/speech/app'],['API / 音色说明','https://www.volcengine.com/docs/6561/1598757']], fields: [['volcengine_app_id','App ID'],['volcengine_key','Access Token','password'],['volcengine_resource','Resource ID'],['volcengine_voice_zh','中文音色 ID'],['volcengine_voice_en','英文音色 ID']] },
  tencent: { name: '腾讯云 TTS', note: '使用 TextToVoice 与 TC3 签名。SecretId / SecretKey 和数字 VoiceType 需拥有语音合成权限。所选文字会发送到腾讯云。', links: [['开通 / 控制台','https://console.cloud.tencent.com/tts'],['API / 音色列表','https://cloud.tencent.com/document/product/1073/37995'],['价格','https://cloud.tencent.com/product/tts/pricing']], fields: [['tencent_secret_id','SecretId'],['tencent_secret_key','SecretKey','password'],['tencent_region','地域'],['tencent_voice_zh','中文 VoiceType'],['tencent_voice_en','英文 VoiceType']] },
  elevenlabs: { name: 'ElevenLabs', note: 'Eleven v3 注重情感表达，可用 [laughs]、[whispers] 等音频标签；Flash v2.5 支持 32 种语言、偏重低延迟。速度限制与费用依模型和套餐变化。所选文字会发送到 ElevenLabs。', links: [['获取 API Key','https://elevenlabs.io/app/settings/api-keys'],['模型 / 音色文档','https://elevenlabs.io/docs/overview/models'],['价格','https://elevenlabs.io/pricing/api']], fields: [['elevenlabs_key','API Key','password'],['elevenlabs_model','模型'],['elevenlabs_voice_zh','中文 Voice ID'],['elevenlabs_voice_en','英文 Voice ID']] }
};
let config: Record<string,string> = {};
let voices: {id:string;name:string}[] = [];
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
    if(key.startsWith('sapi_voice')||key==='elevenlabs_model'){
      const select=document.createElement('select');input=select;
      const options=key==='elevenlabs_model'?[{id:'eleven_flash_v2_5',name:'Flash v2.5 · 快速'},{id:'eleven_v3',name:'Eleven v3 · 情感'}]:[{id:'',name:'按语言自动选择'},...voices];
      for(const o of options)select.add(new Option(o.name,o.id));
      if(config[key]&&!options.some(o=>o.id===config[key]))select.add(new Option(config[key]+'（当前配置）',config[key]));
    }else{input=document.createElement('input');input.type=type??'text';input.autocomplete='off';input.spellcheck=false;}
    input.value=config[key]??'';input.id=`speech-${key}`;
    input.addEventListener('change',()=>{config[key]=input.value.trim();updateConfig(`speech.${key}`,config[key]);});row.append(input);root.append(row);
  }
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
  provider.addEventListener('change',()=>{config.provider=provider.value;updateConfig('speech.provider',provider.value);renderFields();});
  document.getElementById('speech-enabled')?.addEventListener('change',e=>updateConfig('speech.enabled',String((e.target as HTMLInputElement).checked)));
  for(const key of ['rate','volume'])document.getElementById(`speech-${key}`)?.addEventListener('change',e=>updateConfig(`speech.${key}`,(e.target as HTMLInputElement).value));
  document.getElementById('speechPreview')?.addEventListener('click',()=>{const text=(document.getElementById('speechText') as HTMLTextAreaElement).value.trim();const lang=(document.getElementById('speechLanguage') as HTMLSelectElement).value as 'zh'|'en';if(text)post('preview',text,lang);});
  document.getElementById('speechStop')?.addEventListener('click',()=>post('stop'));
  onHostMessage('speechResponse',message=>{if(message.type!=='speechResponse')return;const el=document.getElementById('speechStatus');if(el)el.textContent=message.message;
    const next=message.voices as {id:string;name:string}[];if(JSON.stringify(next)!==JSON.stringify(voices)){voices=next;renderFields();}});
  const panel=document.getElementById('speechPanel') as HTMLDetailsElement;let timer=0;
  const updatePolling=()=>{clearInterval(timer);if(panel.open){post('status');timer=window.setInterval(()=>{if(!document.hidden)post('status');},2000);}};
  panel.addEventListener('toggle',updatePolling);
  window.addEventListener('beforeunload',()=>clearInterval(timer));
  applySpeechConfig(config);updatePolling();setupLocalTts();
}
