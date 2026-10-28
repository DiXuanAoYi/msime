import { afterEach, expect, it, vi } from 'vitest';
import { activateVoicePane, setupVoiceNavigation } from './voice-navigation';
import { readFileSync } from 'node:fs';
afterEach(()=>vi.unstubAllGlobals());
it('keeps each live workflow and its original controls in the pane of its tab',()=>{
  const html=readFileSync(new URL('../partials/voice.html',import.meta.url),'utf8');
  const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(x=>x[1]);
  expect(new Set(ids).size).toBe(ids.length);
  const panes=['speech','studio','recognition','polish','models'];
  const required=[['speech-provider'],['studioText','studioGenerate','studioRoute'],['voiceEnabled','voiceAsrProviderBtn'],['voicePolishText','voicePromptLibrary','voicePolishPrompt'],['localAsrPanel','localTtsPanel','localTtsModels']];
  panes.forEach((name,index)=>{
    const start=html.indexOf(`id="voicePane-${name}"`), end=index+1<panes.length?html.indexOf(`id="voicePane-${panes[index+1]}"`):html.length;
    expect(start).toBeGreaterThan(0);
    for(const id of required[index])expect(html.slice(start,end)).toContain(`id="${id}"`);
  });
});
it('shows recognition and polishing together under 语音输入 and the local models under their own tab',()=>{
  const html=readFileSync(new URL('../partials/voice.html',import.meta.url),'utf8');
  const tabs=[...html.matchAll(/data-voice-target="([a-z]+)"[^>]*>([^<]+)</g)].map(x=>[x[1],x[2]]);
  expect(tabs).toEqual([['input','语音输入'],['tts','朗读发音'],['studio','语音消息'],['models','本地模型']]);
  expect(html.match(/data-voice-pane="input"/g)).toHaveLength(2);
  expect(html.split('<details').length).toBe(html.split('</details>').length);
  expect(html.split('<div').length).toBe(html.split('</div>').length);
});
it('switches existing panes without replacing controls and keeps studio deep links visible',()=>{
  const names=['tts','studio','input','input','models'];
  const panes=names.map(name=>({dataset:{voicePane:name},hidden:false}));
  const targets=['input','tts','studio','models'];
  const tabs=targets.map(name=>({dataset:{voiceTarget:name},attributes:{} as Record<string,string>,events:new Map<string,any>(),tabIndex:0,focus:vi.fn(),setAttribute(key:string,value:string){this.attributes[key]=value;},addEventListener(key:string,callback:any){this.events.set(key,callback);}}));
  const studio={open:false};
  const root={dataset:{} as Record<string,string>,querySelectorAll:(selector:string)=>selector==='[data-voice-pane]'?panes:tabs};
  vi.stubGlobal('document',{getElementById:(id:string)=>id==='voice-settings'?root:id==='voiceStudioPanel'?studio:null});
  setupVoiceNavigation();
  expect(panes.map(x=>x.hidden)).toEqual([true,true,false,false,true]);
  tabs[2].events.get('click')();expect(studio.open).toBe(true);
  expect(panes.map(x=>x.hidden)).toEqual([true,false,true,true,true]);
  expect(tabs[2].attributes['aria-selected']).toBe('true');
  tabs[2].events.get('keydown')({key:'ArrowRight',preventDefault:vi.fn()});
  expect(root.dataset.voicePane).toBe('models');expect(tabs[3].focus).toHaveBeenCalledOnce();
  activateVoicePane('invalid');expect(root.dataset.voicePane).toBe('models');
});
