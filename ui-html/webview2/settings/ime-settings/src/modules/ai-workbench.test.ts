import {afterEach,beforeEach,expect,it,vi} from 'vitest';
const handlers=vi.hoisted(()=>new Map<string,(value:any)=>void>());
vi.mock('../utils/host-messages',()=>({onHostMessage:(type:string,cb:(value:any)=>void)=>handlers.set(type,cb)}));
vi.mock('./shared',()=>({applyToggleState:vi.fn(),setupToggleButton:vi.fn()}));
vi.mock('./config-sync',()=>({updateConfig:vi.fn()}));
import {applyAiTaskConfig,setupAiWorkbench} from './ai-workbench';
class Element extends EventTarget {
 value='';textContent='';type='';dataset:Record<string,string>={};children:Element[]=[];tag='';
 setAttribute(){}append(...nodes:Element[]){this.children.push(...nodes);}replaceChildren(){this.children=[];}querySelectorAll(selector:string):Element[]{return this.children.flatMap(x=>[...(x.tag===selector?[x]:[]),...x.querySelectorAll(selector)]);}click(){this.dispatchEvent(new Event('click'));}
}
let nodes:Map<string,Element>,post:ReturnType<typeof vi.fn>;
beforeEach(()=>{handlers.clear();nodes=new Map(['aiTask','aiTaskInput','aiTaskContext','aiTaskRun','aiTaskCancel','aiTaskStatus','aiTaskResults'].map(x=>[x,new Element()]));post=vi.fn();vi.stubGlobal('window',{chrome:{webview:{postMessage:post}}});vi.stubGlobal('document',{getElementById:(id:string)=>nodes.get(id),createElement:(tag:string)=>Object.assign(new Element(),{tag})});setupAiWorkbench();});
afterEach(()=>vi.unstubAllGlobals());
const request=()=>JSON.parse(post.mock.calls.at(-1)![0]);
it('question uses no application context and does not send replies automatically',()=>{applyAiTaskConfig({task:'question'});nodes.get('aiTaskInput')!.value='三个直是什么字';nodes.get('aiTaskContext')!.value='private chat';nodes.get('aiTaskRun')!.click();const req=request();expect(req.data.context).toBe('');handlers.get('aiTaskResult')!({requestId:req.data.requestId,ok:true,task:'question',message:'ok',completion:'',answers:[{text:'矗',pinyin:'chù',source:'本地表'}]});expect(nodes.get('aiTaskResults')!.children).toHaveLength(1);expect(nodes.get('aiTaskResults')!.querySelectorAll('textarea')[0].value).toBe('矗');expect(post).toHaveBeenCalledTimes(1);});
it('new output preserves edited prior drafts and is manually copied only',()=>{applyAiTaskConfig({task:'completion'});nodes.get('aiTaskRun')!.click();let req=request();const response=(id:string,text:string)=>({requestId:id,ok:true,task:'completion',message:'ok',completion:text,answers:[]});handlers.get('aiTaskResult')!(response(req.data.requestId,'初稿'));const old=nodes.get('aiTaskResults')!.querySelectorAll('textarea')[0];old.value='我编辑过的内容';nodes.get('aiTaskRun')!.click();req=request();handlers.get('aiTaskResult')!(response(req.data.requestId,'新稿'));expect(nodes.get('aiTaskResults')!.querySelectorAll('textarea').map(x=>x.value)).toEqual(['我编辑过的内容','新稿']);expect(post).toHaveBeenCalledTimes(2);nodes.get('aiTaskResults')!.children[0].children[2].click();expect(request()).toMatchObject({type:'copyText',data:'我编辑过的内容'});});
it('cancel and stale responses never replace an existing draft',()=>{applyAiTaskConfig({task:'completion'});nodes.get('aiTaskRun')!.click();const req=request();nodes.get('aiTaskCancel')!.click();handlers.get('aiTaskResult')!({requestId:req.data.requestId,ok:true,task:'completion',message:'late',completion:'late',answers:[]});expect(nodes.get('aiTaskResults')!.children).toHaveLength(0);expect(request().data.action).toBe('cancel');});
