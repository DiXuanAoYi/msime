export type VoiceChoice = { id: string; name: string; language?: string; dialect?: string };
export type ModelControl = { id: string; label: string; type: 'int'|'float'|'bool'|'text'|'enum'; min?:number; max?:number; step?:number; default?:unknown; values?:string[]; maxBytes?:number };
export type SpeechModel = { id?:string; name?:string; voices?:VoiceChoice[]; controls?:ModelControl[]; languages?:string[]; dialectNote?:string; parameterNote?:string };
export const LANGUAGE_NAMES:Record<string,string>={zh:'中文',en:'英语',ja:'日语',ko:'韩语',fr:'法语',de:'德语',es:'西班牙语',it:'意大利语',pt:'葡萄牙语',ru:'俄语',nl:'荷兰语',ar:'阿拉伯语',hi:'印地语',fa:'波斯语',hu:'匈牙利语',pl:'波兰语',cs:'捷克语',da:'丹麦语',sv:'瑞典语',el:'希腊语',tr:'土耳其语',vi:'越南语'};
Object.assign(LANGUAGE_NAMES,{bo:'藏语',kk:'哈萨克语',ug:'维吾尔语',mn:'蒙古语'});
export function parseModelOptions(raw:string|undefined):Record<string,Record<string,unknown>>{
  try { const v=JSON.parse(raw??'{}');return v && typeof v==='object' && !Array.isArray(v)?v:{}; } catch {return {};}
}
export function fillVoices(select:HTMLSelectElement, voices:VoiceChoice[], current:string):void{
  select.replaceChildren();
  for(const voice of voices)select.add(new Option(`${voice.name}${voice.dialect?' · '+voice.dialect:''}`,voice.id));
  if(!voices.some(v=>v.id===current))select.add(new Option(current?`${current}（当前配置）`:'按模型自动选择',current));
  select.value=current;
}
export function renderModelControls(root:HTMLElement, model:SpeechModel, values:Record<string,unknown>, changed:(values:Record<string,unknown>)=>void):void{
  root.replaceChildren();
  const note=document.createElement('p');note.className='voice-description';note.textContent=(model.dialectNote??'仅显示当前模型已接入的参数；其他参数保持运行组件默认值。')+(model.parameterNote?' '+model.parameterNote:'');root.append(note);
  const grid=document.createElement('div');grid.className='speech-fields';root.append(grid);
  for(const item of model.controls??[]){
    const label=document.createElement('label');label.className='speech-field';label.append(document.createTextNode(item.label));
    let input:HTMLInputElement|HTMLSelectElement;
    if(item.type==='enum'){
      const select=document.createElement('select');for(const value of item.values??[])select.add(new Option(value,value));input=select;
    }else{
      const e=document.createElement('input');input=e;e.type=item.type==='bool'?'checkbox':item.type==='text'?'text':'number';
      if(item.min!==undefined)e.min=String(item.min);if(item.max!==undefined)e.max=String(item.max);
      e.step=String(item.step??(item.type==='int'?1:0.05));
      if(item.type==='text')e.maxLength=item.maxBytes??1000;
    }
    const value=values[item.id]??item.default;
    if(item.type==='bool')(input as HTMLInputElement).checked=value===true;else input.value=String(value??'');
    input.addEventListener('change',()=>{
      let value:unknown=input.value;
      if(item.type==='bool')value=(input as HTMLInputElement).checked;
      if(item.type==='int'||item.type==='float'){
        value=Number(input.value);
        if(!Number.isFinite(value as number)||(value as number)<item.min!||(value as number)>item.max!||(item.type==='int'&&!Number.isInteger(value)))return;
      }
      values={...values,[item.id]:value};changed(values);
    });label.append(input);grid.append(label);
  }
  if(model.controls?.length){const reset=document.createElement('button');reset.type='button';reset.className='credential-test-button';reset.textContent='恢复此模型参数默认值';reset.addEventListener('click',()=>{changed({});renderModelControls(root,model,{},changed);});root.append(reset);}
}
