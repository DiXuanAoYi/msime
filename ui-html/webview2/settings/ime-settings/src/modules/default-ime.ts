import {serializeHostMessage} from '../../../../shared/messages';
import {onHostMessage} from '../utils/host-messages';
export function setupDefaultIme():void {
 const button=document.getElementById('defaultImeSetButton') as HTMLButtonElement|null,system=document.getElementById('defaultImeSystemButton'),status=document.getElementById('defaultImeStatus');if(!button||!status)return;
 let pending='',timer:ReturnType<typeof setTimeout>|undefined;
 const send=(action:'status'|'setDefault'|'openSystemSettings')=>{pending=`default-${Date.now()}`;button.disabled=true;status.textContent=action==='setDefault'?'正在把知语移到中文键盘首位…':'正在检查当前顺序…';window.chrome?.webview?.postMessage(serializeHostMessage({type:'defaultImeRequest',data:{requestId:pending,action}}));clearTimeout(timer);timer=setTimeout(()=>{button.disabled=false;status.textContent='查询或设置超时，可打开系统设置手动调整。';pending='';},20000);};
 button.addEventListener('click',()=>send('setDefault'));system?.addEventListener('click',()=>send('openSystemSettings'));
 onHostMessage('defaultImeResponse',r=>{if(r.requestId!==pending)return;pending='';clearTimeout(timer);button.disabled=r.isDefault&&r.ok;button.textContent=r.isDefault?'中文键盘已在首位':'设为中文默认';status.textContent=r.message|| (r.isDefault?'知语已排在中文（简体）键盘首位。':r.enabled?'已加入中文键盘，可设为首位。':'尚未加入当前用户中文键盘；设为默认时会尝试添加，失败可在系统设置调整。');});send('status');
}
