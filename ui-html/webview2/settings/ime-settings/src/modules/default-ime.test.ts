import {afterEach,beforeEach,expect,it,vi} from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(value:any)=>void>());
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string,cb:(value:any)=>void)=>handlers.set(type,cb)}));
import {setupDefaultIme} from './default-ime';
class El extends EventTarget {disabled=false;textContent='';click(){this.dispatchEvent(new Event('click'));}}
let nodes:Map<string,El>,post:ReturnType<typeof vi.fn>;
beforeEach(()=>{handlers.clear();vi.useFakeTimers();nodes=new Map(['defaultImeSetButton','defaultImeSystemButton','defaultImeStatus'].map(x=>[x,new El()]));post=vi.fn();vi.stubGlobal('document',{getElementById:(id:string)=>nodes.get(id)});vi.stubGlobal('window',{chrome:{webview:{postMessage:post}}});});
afterEach(()=>{vi.useRealTimers();vi.unstubAllGlobals();});
const last=()=>JSON.parse(post.mock.calls.at(-1)![0]);
it('only queries status on page setup; changing default requires explicit click',()=>{setupDefaultIme();expect(last().data.action).toBe('status');const req=last();handlers.get('defaultImeResponse')!({requestId:req.data.requestId,action:'status',ok:true,message:'',enabled:true,isDefault:false});expect(nodes.get('defaultImeSetButton')!.disabled).toBe(false);expect(post).toHaveBeenCalledTimes(1);nodes.get('defaultImeSetButton')!.click();expect(last().data.action).toBe('setDefault');});
it('states the Chinese order semantics and handles timeout without claiming success',()=>{setupDefaultIme();const req=last();handlers.get('defaultImeResponse')!({requestId:req.data.requestId,action:'status',ok:true,message:'',enabled:true,isDefault:true});expect(nodes.get('defaultImeStatus')!.textContent).toContain('中文（简体）');nodes.get('defaultImeSystemButton')!.click();expect(last().data.action).toBe('openSystemSettings');vi.advanceTimersByTime(20000);expect(nodes.get('defaultImeStatus')!.textContent).toContain('超时');expect(nodes.get('defaultImeSetButton')!.disabled).toBe(false);});
