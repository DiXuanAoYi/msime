import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';
import { updateConfig } from './config-sync';
import { setupToggleButton,applyToggleState } from './shared';
type Task='pinyin'|'completion'|'question';
let pending='',selected:Task='pinyin',seq=0;
export function setupAiWorkbench():void {
  if(!document.getElementById('aiTaskResults'))return;
  setupToggleButton('aiCompletionEnabled',v=>updateConfig('ai_assistant.completion_enabled',v));
  setupToggleButton('aiQuestionEnabled',v=>updateConfig('ai_assistant.question_enabled',v));
  const task=document.getElementById('aiTask') as HTMLSelectElement|null;
  task?.addEventListener('change',()=>{selected=task.value as Task;updateConfig('ai_assistant.task',selected);
    const text=document.getElementById('aiTaskInput') as HTMLTextAreaElement|null;if(text)text.placeholder=selected==='question'?'例如：三个木是什么字 / 三个直怎么读':selected==='completion'?'请输入已写好的文字，将生成后续新增文字':'例如：ni hao / ming tian kai hui';});
  document.getElementById('aiTaskRun')?.addEventListener('click',()=>{
    if(!window.chrome?.webview)return;pending=`task-${Date.now()}-${++seq}`;
    const text=(document.getElementById('aiTaskInput') as HTMLTextAreaElement|null)?.value??'';
    const context=selected==='question'?'':(document.getElementById('aiTaskContext') as HTMLTextAreaElement|null)?.value??'';
    const status=document.getElementById('aiTaskStatus');if(status)status.textContent='处理中，可取消；不会发送消息';
    window.chrome.webview.postMessage(serializeHostMessage({type:'aiTaskRequest',data:{requestId:pending,action:'run',task:selected,text,context}}));
  });
  document.getElementById('aiTaskCancel')?.addEventListener('click',()=>{if(!window.chrome?.webview)return;
    pending='';window.chrome.webview.postMessage(serializeHostMessage({type:'aiTaskRequest',data:{requestId:`cancel-${++seq}`,action:'cancel',task:selected}}));
    const status=document.getElementById('aiTaskStatus');if(status)status.textContent='已取消，编辑过的结果保留';});
  onHostMessage('aiTaskResult',r=>{if(r.requestId!==pending)return;pending='';
    const status=document.getElementById('aiTaskStatus');if(status)status.textContent=r.message;
    if(!r.ok)return;const output=document.getElementById('aiTaskResults');if(!output)return;
    const dirty=Array.from(output.querySelectorAll<HTMLTextAreaElement>('textarea')).some(x=>x.value!==x.dataset.original);
    if(!dirty)output.replaceChildren();else if(status)status.textContent+='；旧编辑稿已保留，新结果追加在下方';
    const rows=r.task==='completion'?[{text:r.completion,pinyin:'',source:'文本续写'}]:r.answers;
    rows.forEach(row=>{const card=document.createElement('div');card.className='ai-answer';const reading=document.createElement('span');
      reading.textContent=[row.pinyin,row.source].filter(Boolean).join(' · ');
      const draft=document.createElement('textarea');draft.value=row.text;draft.dataset.original=row.text;draft.setAttribute('aria-label','可编辑结果');
      const copy=document.createElement('button');copy.type='button';copy.textContent='复制';copy.addEventListener('click',()=>{
        window.chrome?.webview?.postMessage(serializeHostMessage({type:'copyText',data:draft.value}));if(status)status.textContent='已复制，请手动粘贴；不会发送消息';});
      card.append(reading,draft,copy);output.append(card);});
  });
}
export function applyAiTaskConfig(config:Record<string,unknown>):void {
  if(config.task==='pinyin'||config.task==='completion'||config.task==='question')selected=config.task;
  const task=document.getElementById('aiTask') as HTMLSelectElement|null;if(task)task.value=selected;
  if(typeof config.completion_enabled==='boolean')applyToggleState('aiCompletionEnabled',config.completion_enabled);
  if(typeof config.question_enabled==='boolean')applyToggleState('aiQuestionEnabled',config.question_enabled);
}
