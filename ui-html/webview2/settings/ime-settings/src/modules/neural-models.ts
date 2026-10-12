import {serializeHostMessage} from '../../../../shared/messages';
import {onHostMessage} from '../utils/host-messages';
type Id='desktop'|'tongbian'|'zhiwei';
type Action='status'|'download'|'import'|'cancel'|'delete'|'start'|'stop'|'activate';
type Row={id:Id;name:string;state:string;bytes:number;downloadBytes:number;url:string;license:string;capability:string;note:string;message:string;downloaded:number;total:number;legacy:boolean;active:boolean};
export function modelSize(bytes:number):string{return bytes>0?`${(bytes/1024/1024).toFixed(2)} MiB (${bytes.toLocaleString()} 字节)`:'导入后显示实际大小';}
export function setupNeuralModels():void {
 const root=document.getElementById('neuralModelCards');if(!root)return;
 let timer:ReturnType<typeof setTimeout>|undefined;let counter=0;
 const send=(action:Action,id?:Id)=>window.chrome?.webview?.postMessage(serializeHostMessage({type:'neuralModelsRequest',data:{requestId:`neural-${++counter}-${Date.now()}`,action,...(id?{id}:{})}}));
 document.getElementById('neuralRefresh')?.addEventListener('click',()=>send('status'));
 onHostMessage('neuralModelsResult',response=>{
  if(!response.requestId.startsWith('neural-'))return;const status=document.getElementById('neuralModelStatus');if(status)status.textContent=response.message||String(response.service.message??'');
  root.replaceChildren();const service=response.service;let busy=service.state==='loading'||service.state==='running';
  (response.models as Row[]).forEach(row=>{const card=document.createElement('div');card.className='library-card neural-model';const title=document.createElement('h4');title.textContent=row.name;
   const note=document.createElement('p');note.textContent=row.note;const size=document.createElement('small');size.textContent=`下载 ${modelSize(row.downloadBytes)} · 已安装 ${modelSize(row.bytes)} · ${row.license}`;
   const state=document.createElement('p');state.textContent=(service.model===row.id&&busy?String(service.message??''):row.message)||({absent:'未下载',ready:'已校验',legacy:'使用原数据目录文件（不会迁移或删除）',downloading:'正在下载',error:'发生错误'}[row.state]??row.state);
   const actions=document.createElement('div');actions.className='ai-actions';const button=(text:string,action:Action,disabled=false)=>{const b=document.createElement('button');b.type='button';b.textContent=text;b.disabled=disabled;b.addEventListener('click',()=>{send(action,row.id);clearTimeout(timer);timer=setTimeout(()=>send('status'),1000);});return b;};
   const pending=row.state==='downloading'||row.state==='importing';const ready=row.state==='ready'||row.state==='legacy';busy=busy||pending;
   const running=service.model===row.id&&service.state==='running',current=row.active;
   actions.append(button('下载所选模型','download',ready||pending),button('导入本地文件','import',pending),button(pending?'取消下载':'删除托管文件',pending?'cancel':'delete',row.legacy||(!pending&&!ready)||current));
   actions.append(button(current?'正在使用 · 点击停止':running?'使用模型':'启动并使用',current?'stop':running?'activate':'start',!ready||service.state==='loading'));
   if(running&&!current)actions.append(button('停止服务','stop'));
   const source=document.createElement('a');source.href=row.url;source.target='_blank';source.rel='noopener noreferrer';source.textContent='官方下载网址';card.append(title,size,note,state,actions,source);root.append(card);
  });
  clearTimeout(timer);if(busy)timer=setTimeout(()=>send('status'),1500);
 });send('status');
}
