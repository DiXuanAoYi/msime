import { afterEach, beforeEach, expect, it, vi } from 'vitest';

const handlers = vi.hoisted(() => new Set<(message: any) => void>());
vi.mock('../utils/host-messages', () => ({
  onHostMessage: (_type: string, handler: (message: any) => void) => {
    handlers.add(handler);
    return () => handlers.delete(handler);
  }
}));

class ElementStub {
  value = '';
  textContent = '';
  disabled = false;
  events = new Map<string, (event?: any) => void>();
  addEventListener(name: string, callback: (event?: any) => void) { this.events.set(name, callback); }
  emit(name: string) { this.events.get(name)?.(); }
}
let elements: Map<string, ElementStub>;
let module: typeof import('./personal-glossary-editor');
const get = (id: string) => elements.get(id)!;

beforeEach(async () => {
  vi.resetModules();
  vi.useFakeTimers();
  handlers.clear();
  elements = new Map(['personalGlossaryText', 'personalGlossaryStatus', 'personalGlossarySave',
    'personalGlossaryFile', 'personalGlossaryImport', 'personalGlossaryExport', 'translationTargetLanguageMenu']
    .map(id => [id, new ElementStub()]));
  vi.stubGlobal('document', { getElementById: (id: string) => elements.get(id) });
  vi.stubGlobal('window', { chrome: { webview: {} }, setTimeout, clearTimeout });
  module = await import('./personal-glossary-editor');
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

it('does not replace an unsaved draft when an unrelated setting snapshot arrives', () => {
  module.setupPersonalGlossaryEditor(vi.fn());
  module.applyPersonalGlossaryConfig('en\t苹果\tapple', 'en');
  get('personalGlossaryText').value = 'ja\t猫\t猫';
  get('personalGlossaryText').emit('input');
  module.applyPersonalGlossaryConfig('en\t苹果\tapple', 'ja');
  expect(get('personalGlossaryText').value).toBe('ja\t猫\t猫');
});

it('only confirms saving after the host acknowledges the same glossary', () => {
  const save = vi.fn();
  module.setupPersonalGlossaryEditor(save);
  get('personalGlossaryText').value = 'en\t苹果\tapple';
  get('personalGlossarySave').emit('click');
  expect(save).toHaveBeenCalledWith('en\t苹果\tapple');
  for (const handler of handlers) handler({ data: { general: { personal_translation_glossary: '' } } });
  expect(get('personalGlossarySave').disabled).toBe(true);
  for (const handler of handlers) handler({ data: { general: { personal_translation_glossary: 'en\t苹果\tapple' } } });
  expect(get('personalGlossarySave').disabled).toBe(false);
  expect(get('personalGlossaryStatus').textContent).toContain('已保存');
  expect(handlers.size).toBe(0);
});

it('retains the draft on validation failure or a missing host acknowledgement', () => {
  const save = vi.fn();
  module.setupPersonalGlossaryEditor(save);
  get('personalGlossaryText').value = 'en\t苹果';
  get('personalGlossarySave').emit('click');
  expect(save).not.toHaveBeenCalled();
  expect(get('personalGlossaryStatus').textContent).toContain('第 1 行');
  get('personalGlossaryText').value = 'en\t苹果\tapple';
  get('personalGlossaryText').emit('input');
  get('personalGlossarySave').emit('click');
  vi.advanceTimersByTime(5000);
  expect(get('personalGlossarySave').disabled).toBe(false);
  expect(get('personalGlossaryStatus').textContent).toContain('未收到保存确认');
  expect(get('personalGlossaryText').value).toBe('en\t苹果\tapple');
  expect(handlers.size).toBe(0);
});
it('imports all sense columns, exports the full draft and reimports it without losing meanings', async()=>{
  module.setupPersonalGlossaryEditor(vi.fn());module.applyPersonalGlossaryConfig('','en');
  const source='你好\tint. hello\tint. hi\tphr. how do you do\twelcome\tgreetings\tsalutations\tgood day';
  const encoded=new TextEncoder().encode(source);
  Object.assign(get('personalGlossaryFile'),{files:[{size:encoded.length,arrayBuffer:async()=>encoded.buffer}]});
  await get('personalGlossaryFile').events.get('change')?.();
  const imported=get('personalGlossaryText').value;
  expect(imported).toContain('salutations; good day');
  let exported:Blob|undefined;
  vi.stubGlobal('URL',{createObjectURL:(blob:Blob)=>{exported=blob;return 'blob:owned-fixture';},revokeObjectURL:vi.fn()});
  vi.stubGlobal('document',{getElementById:(id:string)=>elements.get(id),createElement:()=>({href:'',download:'',click:vi.fn()})});
  get('personalGlossaryExport').emit('click');
  expect(await exported?.text()).toBe(imported);
  get('personalGlossaryText').value='';get('personalGlossaryText').emit('input');
  const bytes=new TextEncoder().encode(await exported!.text());
  Object.assign(get('personalGlossaryFile'),{files:[{size:bytes.length,arrayBuffer:async()=>bytes.buffer}]});
  await get('personalGlossaryFile').events.get('change')?.();
  expect(get('personalGlossaryText').value).toBe(imported);
});
