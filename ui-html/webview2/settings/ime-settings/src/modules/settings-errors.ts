import {onHostMessage} from '../utils/host-messages';
export function setupSettingsErrors():void{
 const status=document.getElementById('settings-operation-error');
 const message=document.getElementById('settings-operation-error-message');
 if(!status||!message)return;
 document.getElementById('settings-operation-error-close')?.addEventListener('click',()=>status.hidden=true);
 window.addEventListener('settings-handler-error',()=>{
  message.textContent='一处界面更新失败，其他功能仍可使用。当前编辑内容未改动；可保存或复制后重新打开设置重试。';status.hidden=false;
 });
 onHostMessage('settingsOperationError',response=>{
  message.textContent=response.message;status.hidden=false;
  // Nonmodal: never move focus out of an editor or native file dialog, and never change a draft.
 });
}
