import { afterEach, expect, it, vi } from 'vitest';
import { activateVoicePane, setupVoiceNavigation } from './voice-navigation';
import { readFileSync } from 'node:fs';
afterEach(()=>vi.unstubAllGlobals());
it('keeps each live workflow and its original controls in one distinct pane',()=>{
  const html=readFileSync(new URL('../partials/voice.html',import.meta.url),'utf8');
  const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(x=>x[1]);
  expect(new Set(ids).size).toBe(ids.length);
  const panes=['speech','studio','recognition','polish'];
  const required=[['speech-provider','localTtsModels'],['studioText','studioGenerate','studioRoute'],['voiceEnabled','voiceAsrProviderBtn'],['voicePolishText','voicePromptLibrary','voicePolishPrompt']];
  panes.forEach((name,index)=>{
    const start=html.indexOf(`id="voicePane-${name}"`), end=index+1<panes.length?html.indexOf(`id="voicePane-${panes[index+1]}"`):html.length;
    expect(start).toBeGreaterThan(0);
    for(const id of required[index])expect(html.slice(start,end)).toContain(`id="${id}"`);
  });
});
it('switches existing panes without replacing controls and keeps studio deep links visible',()=>{
  const names=['speech','studio','recognition','polish'];
  const panes=names.map(name=>({dataset:{voicePane:name},hidden:false}));
  const tabs=names.map(name=>({dataset:{voiceTarget:name},attributes:{} as Record<string,string>,events:new Map<string,any>(),tabIndex:0,focus:vi.fn(),setAttribute(key:string,value:string){this.attributes[key]=value;},addEventListener(key:string,callback:any){this.events.set(key,callback);}}));
  const studio={open:false};
  const root={dataset:{} as Record<string,string>,querySelectorAll:(selector:string)=>selector==='[data-voice-pane]'?panes:tabs};
  vi.stubGlobal('document',{getElementById:(id:string)=>id==='voice-settings'?root:id==='voiceStudioPanel'?studio:null});
  setupVoiceNavigation();
  expect(panes.map(x=>x.hidden)).toEqual([true,true,false,true]);
  tabs[1].events.get('click')();expect(studio.open).toBe(true);
  expect(panes.map(x=>x.hidden)).toEqual([true,false,true,true]);
  expect(tabs[1].attributes['aria-selected']).toBe('true');
  tabs[1].events.get('keydown')({key:'ArrowRight',preventDefault:vi.fn()});
  expect(root.dataset.voicePane).toBe('recognition');expect(tabs[2].focus).toHaveBeenCalledOnce();
  activateVoicePane('invalid');expect(root.dataset.voicePane).toBe('recognition');
});
