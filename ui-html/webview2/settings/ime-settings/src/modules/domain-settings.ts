import '../styles/modules/domain-settings.css';
import {serializeHostMessage} from '../../../../shared/messages';
import {onHostMessage} from '../utils/host-messages';
import {updateConfig} from './config-sync';
type Pack={id:string;name:string;entries:number;bytes:number;license:string;source:string;version:string;sha256:string;exportable:boolean};
type Profile={application:string;domain_mode?:string;domains?:string[]};
let packs:Pack[]=[],globalDomains:string[]=[],profiles:Profile[]=[];
const initializedCatalogs=new WeakSet<HTMLElement>();
const el=(id:string)=>document.getElementById(id);
const send=(action:'domainCatalog'|'exportDomain'|'setApplicationDomains',extra:Record<string,unknown>={})=>window.chrome?.webview?.postMessage(serializeHostMessage({type:'personalLibraryRequest',data:{requestId:`domains-${action}-${Date.now()}`,action,...extra}}));
function renderApplication():void {
 const app=(el('domainApplication') as HTMLInputElement|null)?.value.trim().toLowerCase()??'';
 const profile=profiles.find(x=>x.application===app),mode=el('domainApplicationMode') as HTMLSelectElement|null;if(mode)mode.value=profile?.domain_mode??'inherit';
 const area=el('domainApplicationPacks');if(!area)return;area.replaceChildren();packs.forEach(pack=>{const label=document.createElement('label'),check=document.createElement('input');check.type='checkbox';check.value=pack.id;check.checked=(mode?.value==='replace'?(profile?.domains??[]):globalDomains).includes(pack.id);check.disabled=mode?.value!=='replace';label.append(check,document.createTextNode(pack.name));area.append(label);});
}
type SourceLink = {label: string; url: string};
const qingjianFiles: Record<string, string> = {
 animals: 'animals.tsv', automotive: 'automotive.tsv', finance: 'finance.tsv', food: 'food.tsv',
 historical: 'historical_figures.tsv', idioms: 'idioms.tsv', it: 'it_computing.tsv', law: 'law.tsv',
 medicine: 'medicine.tsv', places: 'places.tsv', poetry: 'poetry_lines.tsv',
};
function webUrl(value: string): string | null {
 try {
  const url = new URL(value);
  return (url.protocol === 'https:' || url.protocol === 'http:') && !url.username && !url.password ? url.href : null;
 } catch { return null; }
}
export function domainPackSourceLinks(pack: Pick<Pack, 'id' | 'source' | 'version'>): SourceLink[] {
 const sourceUrl = webUrl(pack.source);
 if (sourceUrl) return [{label: '来源', url: sourceUrl}];
 if (!/^qingjian\/THUOCL\b/i.test(pack.source)) return [];
 const filename = qingjianFiles[pack.id];
 const commit = /^[a-f0-9]{40}$/i.test(pack.version) ? pack.version : null;
 return [
  {label: '青简词库', url: filename && commit
   ? `https://github.com/qingjian-team/qingjian/blob/${commit}/assets/lexicon/03_domains/${filename}`
   : 'https://github.com/qingjian-team/qingjian'},
  {label: 'THUOCL', url: 'https://github.com/thunlp/THUOCL'},
  {label: 'Unihan', url: 'https://www.unicode.org/charts/unihan.html'},
 ];
}
export function domainVersionLabel(version: string): string {
 return /^[a-f0-9]{40}$/i.test(version) ? version.slice(0, 8) : (version || '未知');
}
function render(): void {
 const area = el('domainCatalog');
 if (!area) return;
 area.replaceChildren();
 packs.forEach(pack => {
  const row = document.createElement('div');
  row.className = 'library-card domain-pack-card';
  const label = document.createElement('label'), check = document.createElement('input');
  label.className = 'domain-pack-title';
  check.type = 'checkbox';
  check.checked = globalDomains.includes(pack.id);
  check.addEventListener('change', () => {
   globalDomains = check.checked ? Array.from(new Set([...globalDomains, pack.id])) : globalDomains.filter(x => x !== pack.id);
   updateConfig('input.enabled_domain_dictionaries', globalDomains.join(','));
  });
  label.append(check, document.createTextNode(pack.name));
  const metrics = document.createElement('small');
  metrics.className = 'domain-pack-metrics';
  metrics.textContent = `${pack.entries.toLocaleString()} 条 · ${(pack.bytes / 1024 / 1024).toFixed(2)} MiB`;
  const license = document.createElement('small');
  license.textContent = `许可：${pack.license}`;
  const version = document.createElement('small');
  version.className = 'domain-pack-version';
  version.textContent = `版本：${domainVersionLabel(pack.version)}`;
  version.title = `完整版本：${pack.version}\nSHA-256：${pack.sha256}`;
  const actions = document.createElement('div');
  actions.className = 'domain-pack-actions';
  const links = domainPackSourceLinks(pack);
  links.forEach(link => {
   const source = document.createElement('a');
   source.textContent = link.label;
   source.href = link.url;
   source.target = '_blank';
   source.rel = 'noopener noreferrer';
   actions.append(source);
  });
  if (!links.length) {
   const source = document.createElement('small');
   source.className = 'domain-pack-source-note';
   source.textContent = pack.source ? `来源：${pack.source}` : '未提供来源网址';
   row.append(source);
  }
  const exportButton = document.createElement('button');
  exportButton.type = 'button';
  exportButton.textContent = '导出 TSV';
  exportButton.disabled = !pack.exportable;
  exportButton.addEventListener('click', () => send('exportDomain', {id: pack.id}));
  actions.append(exportButton);
  row.prepend(label, metrics, license, version);
  row.append(actions);
  area.append(row);
 });
 renderApplication();
}
export function setupDomainSettings():void {
 const catalog=el('domainCatalog');if(!catalog||initializedCatalogs.has(catalog))return;initializedCatalogs.add(catalog);
 el('domainRefresh')?.addEventListener('click',()=>send('domainCatalog'));
 el('domainApplication')?.addEventListener('change',renderApplication);
 el('domainApplicationMode')?.addEventListener('change',()=>{const mode=(el('domainApplicationMode') as HTMLSelectElement).value;el('domainApplicationPacks')?.querySelectorAll<HTMLInputElement>('input').forEach(x=>x.disabled=mode!=='replace');saveApplication();});
 const saveApplication=()=>{const app=(el('domainApplication') as HTMLInputElement).value.trim().toLowerCase();if(!app)return;const domain_mode=(el('domainApplicationMode') as HTMLSelectElement).value;
 const domains=Array.from(el('domainApplicationPacks')?.querySelectorAll<HTMLInputElement>('input:checked')??[]).map(x=>x.value);send('setApplicationDomains',{application:app,domain_mode,domains});};
 el('domainApplicationPacks')?.addEventListener('change',saveApplication);
 onHostMessage('personalLibraryResponse', r => {
  const status = el('domainStatus');
  // Optional pack failures must be visible even when the successful request belongs to the original library page.
  if (typeof r.domainWarning === 'string' && r.domainWarning) {
   if (status) status.textContent = r.domainWarning;
   packs = [];
   render();
  }
  if (!r.requestId.startsWith('domains-')) return;
  if (status) status.textContent = [r.message, r.domainWarning].filter(Boolean).join('；');
  if (!r.ok) {
   if (r.requestId.startsWith('domains-domainCatalog-')) {
    packs = [];
    render();
   }
   return;
  }
  if (Array.isArray(r.books)) packs = r.books as Pack[];
  if (Array.isArray(r.enabled_domains)) globalDomains = r.enabled_domains;
  if (Array.isArray(r.applications)) profiles = r.applications as Profile[];
  if (typeof r.exported === 'string') {
   const url = URL.createObjectURL(new Blob([r.exported], {type: 'text/tab-separated-values;charset=utf-8'}));
   const a = document.createElement('a');
   a.href = url;
   a.download = typeof r.filename === 'string' && /^msime-domain-[a-z]+\.tsv$/.test(r.filename)
    ? r.filename : '知语-领域输入词库.tsv';
   a.click();
   setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  render();
 });
 send('domainCatalog');
}
