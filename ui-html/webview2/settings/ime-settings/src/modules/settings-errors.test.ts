import {afterEach,expect,it,vi} from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(data:any)=>void>());
vi.mock('../utils/host-messages',()=>({onHostMessage:(name:string,cb:(data:any)=>void)=>handlers.set(name,cb)}));
import {setupSettingsErrors} from './settings-errors';
afterEach(()=>vi.unstubAllGlobals());
it('shows a nonmodal message without focusing, modifying an editor or interrupting native save',()=>{
 vi.stubGlobal('window',new EventTarget());
 const dismiss:Record<string,()=>void>={};const focus=vi.fn();const draft={value:'unsaved all senses'};
 const banner={hidden:true,focus},message={textContent:''};
 vi.stubGlobal('document',{getElementById:(id:string)=>({'settings-operation-error':banner,'settings-operation-error-message':message,'settings-operation-error-close':{addEventListener:(name:string,fn:()=>void)=>dismiss[name]=fn}}[id])});
 setupSettingsErrors();handlers.get('settingsOperationError')!({message:'状态刷新失败，请重试',automatic:true});
 expect(banner.hidden).toBe(false);expect(message.textContent).toContain('重试');expect(focus).not.toHaveBeenCalled();expect(draft.value).toBe('unsaved all senses');
 dismiss.click!();expect(banner.hidden).toBe(true);
});
it('reports a failed UI handler without exposing its payload or modifying a draft',()=>{
 const win=new EventTarget(),focus=vi.fn(),draft={value:'unsaved draft'};
 const banner={hidden:true,focus},message={textContent:''};
 vi.stubGlobal('window',win);
 vi.stubGlobal('document',{getElementById:(id:string)=>({'settings-operation-error':banner,'settings-operation-error-message':message,'settings-operation-error-close':{addEventListener:vi.fn()}}[id])});
 setupSettingsErrors();win.dispatchEvent(new CustomEvent('settings-handler-error',{detail:{type:'configSnapshot'}}));
 expect(banner.hidden).toBe(false);expect(message.textContent).toContain('界面更新失败');
 expect(message.textContent).not.toContain('configSnapshot');expect(focus).not.toHaveBeenCalled();expect(draft.value).toBe('unsaved draft');
});
