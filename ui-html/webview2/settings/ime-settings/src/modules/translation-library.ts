import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage, type ServerMessage } from '../../../../shared/messages';
type Book = Extract<ServerMessage, { type: 'dictionaryLibraryResponse' }>['books'][number];
export function setupTranslationLibrary(): void {
  const root = document.getElementById('translationLibrary');
  if (!root || root.dataset.ready) return;
  root.dataset.ready = 'true';
  let books: Book[] = [], disabled = new Set<string>(), pending = false;
  let saveTimer: ReturnType<typeof setTimeout> | undefined;
  const requests = new Map<string, 'list'|'export'>();
  const status = root.querySelector<HTMLElement>('[data-status]')!;
  const list = root.querySelector<HTMLElement>('[data-books]')!;
  const search = root.querySelector<HTMLInputElement>('[data-search]')!;
  const category = root.querySelector<HTMLSelectElement>('[data-category]')!;
  function request(action: 'list'|'export', id?: string) {
    const requestId = `library-${Date.now()}-${Math.random()}`;
    requests.set(requestId, action);
    window.chrome?.webview?.postMessage(serializeHostMessage({type:'dictionaryLibraryRequest', data:{requestId,action,...(id ? {id} : {})}}));
  }
  function render() {
    list.replaceChildren();
    const visible = books.filter(b=>(category.value==='all'||b.category===category.value) && `${b.name} ${b.language} ${b.category}`.toLowerCase().includes(search.value.toLowerCase()));
    for (const b of visible) {
      const card = document.createElement('div');card.className='library-book';
      const label=document.createElement('label'), toggle=document.createElement('input');toggle.type='checkbox';toggle.checked=b.enabled;toggle.disabled=pending;
      toggle.setAttribute('aria-label',`启用${b.name}`);
      toggle.onchange=()=>{
        clearTimeout(saveTimer);
        saveTimer = setTimeout(()=>{pending=false;render();status.textContent='未收到保存确认，请刷新核对开关状态。';}, 5000);
        pending=true; toggle.checked ? disabled.delete(b.id):disabled.add(b.id);render();status.textContent='正在保存词典开关…';
        window.chrome?.webview?.postMessage(serializeHostMessage({type:'configUpdate',data:{path:'general.disabled_translation_dictionaries',value:[...disabled].sort().join(',')}}));
      };
      label.append(toggle,document.createTextNode(b.name));card.append(label);
      const size=b.bytes<1024*1024 ? `${(b.bytes/1024).toFixed(1)} KB` : `${(b.bytes/1024/1024).toFixed(1)} MB`;
      const details=document.createElement('p');details.textContent=`${b.language} · ${b.headwords.toLocaleString()} 源词条 · ${b.entries.toLocaleString()} 检索条目 · ${size}`;
      card.append(details);
      const note=document.createElement('small');note.textContent=b.note;card.append(note);
      const actions=document.createElement('div');actions.className='library-book-actions';
      const source=document.createElement('a');source.textContent=`来源 · ${b.license}`;if (/^https?:\/\//i.test(b.source)) source.href=b.source;source.target='_blank';source.rel='noopener noreferrer';
      const exportButton=document.createElement('button');exportButton.type='button';exportButton.className='dict-button secondary';exportButton.textContent='导出 TSV…';exportButton.onclick=()=>request('export',b.id);
      actions.append(source,exportButton);card.append(actions);list.append(card);
    }
    if(!visible.length)list.textContent='没有匹配的词典。';
  }
  search.oninput=render;category.onchange=render;
  root.querySelector<HTMLButtonElement>('[data-refresh]')!.onclick=()=>request('list');
  onHostMessage('configSnapshot',()=>{if(pending)request('list');});
  onHostMessage('dictionaryLibraryResponse',message=>{
    const action=requests.get(message.requestId);
    if(!action)return;
    requests.delete(message.requestId);
    if(action==='export'){status.textContent=message.message;return;}
    clearTimeout(saveTimer);pending=false;
    if(message.ok){books=message.books;disabled=new Set(books.filter(b=>!b.enabled).map(b=>b.id));render();}
    render();
    status.textContent=message.message || `${books.filter(b=>b.enabled).length} / ${books.length} 本已启用。关闭后不再参与候选翻译，重叠词仍可能由其他已启用词典提供。`;
  });
  request('list');
}
