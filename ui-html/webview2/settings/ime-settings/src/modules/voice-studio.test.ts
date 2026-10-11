import { afterEach, expect, it, vi } from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(value:any)=>void>());
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string,callback:any)=>handlers.set(type,callback)}));
import { setupVoiceStudio, applyVoiceStudioConfig } from './voice-studio';
import { parseModelOptions, renderModelControls } from './speech-model-options';

class Element {
  value='';textContent='';type='';id='';className='';hidden=false;disabled=false;open=false;checked=false;
  dataset:Record<string,string>={};children:Element[]=[];options:Element[]=[];events=new Map<string,()=>void>();
  append(...elements:Element[]){this.children.push(...elements);}
  appendChild(e:Element){this.children.push(e);}
  replaceChildren(){this.children=[];this.options=[];}
  add(e:Element){this.options.push(e);}
  addEventListener(name:string,fn:()=>void){this.events.set(name,fn);}
  querySelector(){return null;}
  fire(name:string){this.events.get(name)?.();}
}
afterEach(()=>{vi.unstubAllGlobals();vi.useRealTimers();});
it('keeps generated audio tied to the current draft and never sends it without an explicit action',()=>{
  vi.useFakeTimers();handlers.clear();const sent:any[]=[];
  const ids=['voiceStudioPanel','studioText','studioLanguage','studioVoice','studioRate','studioOptions','studioProvider','studioDialect','studioBytes','studioStatus','studioDuration','studioDevice','studioGenerate','studioStop','studioRecord','studioRecordStop','studioImport','studioTranscribe','studioPlay','studioCopy','studioSave','studioRoute','studioCableHelp'];
  const dom=new Map(ids.map(id=>[id,new Element()]));dom.get('voiceStudioPanel')!.open=true;
  vi.stubGlobal('Option',class extends Element{constructor(label:string,value:string){super();this.textContent=label;this.value=value;}});
  vi.stubGlobal('document',{getElementById:(id:string)=>dom.get(id)??null,createElement:()=>new Element(),createTextNode:(text:string)=>{const e=new Element();e.textContent=text;return e;}});
  vi.stubGlobal('window',{chrome:{webview:{postMessage:(value:string)=>sent.push(JSON.parse(value))}},setInterval,addEventListener:vi.fn()});
  setupVoiceStudio();
  applyVoiceStudioConfig({provider:'local',local_model:'qwen-0.6b',local_voice_zh:'Eric',rate:'1',local_options:'{}'},
    {id:'qwen-0.6b',languages:['zh','en'],voices:[{id:'Eric',name:'Eric',language:'zh',dialect:'四川话'}],controls:[]},[]);
  expect(sent.every(m=>m.data.action==='status')).toBe(true);
  dom.get('studioText')!.value='今天练习四川话。';dom.get('studioText')!.fire('input');dom.get('studioGenerate')!.fire('click');
  expect(sent.at(-1).data).toMatchObject({action:'generate',text:'今天练习四川话。',voice:'Eric',language:'zh',rate:1});
  const response=(artifactId:string)=>handlers.get('voiceStudioResponse')!({type:'voiceStudioResponse',ok:true,message:'',state:{phase:'ready',artifactId,transcript:'',inputSeconds:0,outputSeconds:2,hasInput:false,devices:[{id:0,name:'Speakers',virtual:false},{id:3,name:'CABLE Input',virtual:true}]}});
  response('first');expect(dom.get('studioCopy')!.disabled).toBe(false);
  expect(sent.some(m=>['copy','route','send'].includes(m.data.action))).toBe(false);
  dom.get('studioText')!.value='修改后的内容';dom.get('studioText')!.fire('input');
  expect(dom.get('studioCopy')!.disabled).toBe(true);
  const count=sent.length;dom.get('studioCopy')!.fire('click');expect(sent).toHaveLength(count);
  dom.get('studioGenerate')!.fire('click');
  // Editing while synthesis is in progress must not mark the old result fresh.
  dom.get('studioText')!.value='生成期间再次修改';dom.get('studioText')!.fire('input');response('second');
  expect(dom.get('studioCopy')!.disabled).toBe(true);
  dom.get('studioGenerate')!.fire('click');response('third');dom.get('studioCopy')!.fire('click');
  expect(sent.at(-1).data).toMatchObject({action:'copy',artifactId:'third'});
  expect(dom.get('studioDevice')!.options.map(o=>o.value)).toEqual(['','3']);
  dom.get('studioDevice')!.value='3';dom.get('studioDevice')!.fire('change');dom.get('studioRoute')!.fire('click');
  expect(sent.at(-1).data).toMatchObject({action:'route',deviceId:3,artifactId:'third'});
  expect(sent.some(m=>m.type==='commitText'||m.data.action==='send')).toBe(false);
  dom.get('studioRate')!.value='0';dom.get('studioGenerate')!.fire('click');expect(sent.at(-1).data.action).toBe('route');
});
it('preserves model-scoped settings and represents numeric and boolean controls with their real types',()=>{
  const config=parseModelOptions('{"qwen-0.6b":{"temperature":0.6},"moss-nano":{"do_sample":false}}');
  expect(config['moss-nano'].do_sample).toBe(false);expect(parseModelOptions('not json')).toEqual({});
  vi.stubGlobal('document',{createElement:()=>new Element(),createTextNode:()=>new Element()});
  const root=new Element(),changed=vi.fn();
  renderModelControls(root as unknown as HTMLElement,{controls:[{id:'temperature',label:'温度',type:'float',min:0.1,max:1.5,default:0.9},{id:'do_sample',label:'随机采样',type:'bool',default:true}]},{},changed);
  const grid=root.children[1];const temperature=grid.children[0].children[1];temperature.value='0.7';temperature.fire('change');
  expect(changed).toHaveBeenLastCalledWith({temperature:0.7});
  const sample=grid.children[1].children[1];sample.checked=false;sample.fire('change');
  expect(changed).toHaveBeenLastCalledWith({temperature:0.7,do_sample:false});
  temperature.value='99';temperature.fire('change');expect(changed).toHaveBeenCalledTimes(2);
  expect(config['qwen-0.6b'].temperature).toBe(0.6);
});
