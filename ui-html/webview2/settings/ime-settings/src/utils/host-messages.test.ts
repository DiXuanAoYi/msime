import {afterEach,expect,it,vi} from 'vitest';

afterEach(()=>vi.unstubAllGlobals());
it('delivers a validated snapshot to later modules when one module throws without leaking content',async()=>{
 vi.resetModules();
 const webview=new EventTarget(),win=new EventTarget();
 Object.assign(win,{chrome:{webview}});vi.stubGlobal('window',win);
 const {onHostMessage}=await import('./host-messages');
 const errors:unknown[]=[],later=vi.fn();
 win.addEventListener('settings-handler-error',event=>errors.push((event as CustomEvent).detail));
 const unsubscribe=onHostMessage('configSnapshot',()=>{throw new Error('private-exception-must-not-be-reported');});
 onHostMessage('configSnapshot',later);
 const snapshot={type:'configSnapshot',data:{exampleDraft:'private-draft-must-not-be-reported'}};
 webview.dispatchEvent(Object.assign(new Event('message'),{data:snapshot}));
 expect(later).toHaveBeenCalledExactlyOnceWith(snapshot);expect(errors).toEqual([{type:'configSnapshot'}]);
 unsubscribe();webview.dispatchEvent(Object.assign(new Event('message'),{data:snapshot}));
 expect(later).toHaveBeenCalledTimes(2);expect(errors).toHaveLength(1);
});
it('uses a stable subscriber set for each frame and rejects invalid payloads',async()=>{
 vi.resetModules();const webview=new EventTarget(),win=new EventTarget();Object.assign(win,{chrome:{webview}});vi.stubGlobal('window',win);
 const {onHostMessage}=await import('./host-messages');const added=vi.fn();
 let registered=false;onHostMessage('windowState',()=>{if(!registered){registered=true;onHostMessage('windowState',added);}});
 const send=(data:unknown)=>webview.dispatchEvent(Object.assign(new Event('message'),{data}));
 send({type:'windowState',data:{isMaximized:false}});expect(added).not.toHaveBeenCalled();
 send({type:'windowState',data:{isMaximized:true}});expect(added).toHaveBeenCalledOnce();
 send({type:'windowState',data:{isMaximized:'invalid'}});expect(added).toHaveBeenCalledOnce();
});
