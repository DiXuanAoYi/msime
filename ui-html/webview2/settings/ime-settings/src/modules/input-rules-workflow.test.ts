import {afterEach,expect,it,vi} from 'vitest';
const writes=vi.hoisted(()=>vi.fn());
vi.mock('./config-sync',()=>({updateConfig:writes}));
vi.mock('./shared',()=>({applyToggleState:vi.fn(),setupToggleButton:vi.fn()}));
class Element extends EventTarget {
 value='';textContent='';className='';children:Element[]=[];
 append(...children:Element[]):void {this.children.push(...children);}
 replaceChildren(...children:Element[]):void {this.children=[...children];}
 setAttribute():void {}
 add():void {}
}
afterEach(()=>{vi.unstubAllGlobals();writes.mockClear();});
it('rejects invalid and duplicate edited processes without persisting or changing previous rules',async()=>{
 vi.resetModules();const elements=new Map(['englishRules','englishRuleApp','englishRuleAdd','englishRuleStatus'].map(id=>[id,new Element()]));
 vi.stubGlobal('document',{getElementById:(id:string)=>elements.get(id)??null,createElement:()=>new Element()});
 vi.stubGlobal('Option',class {text:string;value:string;constructor(text:string,value:string){this.text=text;this.value=value;}});
 const {setupInputAdvanced,applyInputAdvanced}=await import('./input-advanced');setupInputAdvanced();
 applyInputAdvanced({english_candidate_app_rules:JSON.stringify([{application:'code.exe',mode:'disabled'},{application:'qq.exe',mode:'enabled'}])});
 const app=elements.get('englishRules')!.children[0].children[0];
 app.value='C:/invalid.exe';app.dispatchEvent(new Event('change'));
 expect(writes).not.toHaveBeenCalled();expect(app.value).toBe('code.exe');expect(elements.get('englishRuleStatus')!.textContent).toContain('未保存');
 app.value='QQ.exe';app.dispatchEvent(new Event('change'));expect(writes).not.toHaveBeenCalled();expect(app.value).toBe('code.exe');
 app.value='  Browser.exe  ';app.dispatchEvent(new Event('change'));
 expect(writes).toHaveBeenCalledExactlyOnceWith('input.english_candidate_app_rules',JSON.stringify([{application:'browser.exe',mode:'disabled'},{application:'qq.exe',mode:'enabled'}]));
});
