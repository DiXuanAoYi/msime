import { onHostMessage } from '../utils/host-messages';
import { importQingjianGlossary, MAX_GLOSSARY_BYTES, validatePersonalGlossary } from './personal-glossary';

let draftChanged = false;
let learningLanguage = 'en';
let waitingForSave = false;
const editor = () => document.getElementById('personalGlossaryText') as HTMLTextAreaElement | null;
function status(message: string): void {
  const element = document.getElementById('personalGlossaryStatus');
  if (element) element.textContent = message;
}

export function applyPersonalGlossaryConfig(text: unknown, language: unknown): void {
  if (typeof language === 'string') learningLanguage = language;
  const field = editor();
  if (field && typeof text === 'string' && !draftChanged && !waitingForSave) field.value = text;
}

export function setupPersonalGlossaryEditor(save: (text: string) => void): void {
  const field = editor();
  const saveButton = document.getElementById('personalGlossarySave') as HTMLButtonElement | null;
  if (!field || !saveButton) return;
  field.addEventListener('input', () => { draftChanged = true; status('尚未保存'); });
  document.getElementById('translationTargetLanguageMenu')?.addEventListener('click', (event: Event) => {
    const item = (event.target as HTMLElement | null)?.closest<HTMLElement>('.dropdown-item');
    if (item?.dataset.value) learningLanguage = item.dataset.value;
  });
  saveButton.addEventListener('click', () => {
    const text = field.value;
    const error = validatePersonalGlossary(text);
    if (error) { status(error); return; }
    if (!window.chrome?.webview) { status('请在水杉设置窗口中保存；浏览器预览不会写入配置。'); return; }
    waitingForSave = true;
    saveButton.disabled = true;
    status('正在保存…');
    const unsubscribe = onHostMessage('configSnapshot', (message) => {
      const saved = (message.data as Record<string, any> | undefined)?.general?.personal_translation_glossary;
      if (saved !== text) return;
      finish();
      draftChanged = field.value !== text;
      status(draftChanged ? '已保存；当前还有未保存的修改' : '已保存，下一次候选查询生效');
    });
    const timer = window.setTimeout(() => {
      finish();
      status('未收到保存确认，草稿已保留，请重试。');
    }, 5000);
    function finish(): void {
      window.clearTimeout(timer);
      unsubscribe();
      waitingForSave = false;
      saveButton!.disabled = false;
    }
    save(text);
  });

  const fileInput = document.getElementById('personalGlossaryFile') as HTMLInputElement | null;
  document.getElementById('personalGlossaryImport')?.addEventListener('click', () => fileInput?.click());
  fileInput?.addEventListener('change', async () => {
    const file = fileInput.files?.[0];
    if (!file) return;
    const before = field.value;
    // Capture language before the asynchronous file read.
    const language = learningLanguage;
    try {
      if (file.size > MAX_GLOSSARY_BYTES) throw new Error('导入文件不能超过 64 KB。');
      const text = new TextDecoder('utf-8', { fatal: true }).decode(await file.arrayBuffer());
      // A native multilingual table already has a language column. Otherwise
      // interpret it as Qingjian's per-language personal glossary.
      const firstRow = text.replace(/^\uFEFF/, '').split(/\r?\n/).find(row => row.trim() && !row.trim().startsWith('#')) ?? '';
      const isMultilingual = /^(en|ja|es|fr|de|ru|ko)\t/u.test(firstRow.trim());
      const imported = isMultilingual || validatePersonalGlossary(text) === null ? text : importQingjianGlossary(text, language);
      const combined = before.trimEnd() + (before.trim() && imported.trim() ? '\n' : '') + imported;
      const error = validatePersonalGlossary(combined);
      if (error) throw new Error(error);
      if (field.value !== before) throw new Error('读取期间词表已修改，请重新导入。');
      field.value = combined;
      draftChanged = true;
      status('已加入草稿，请检查后点击保存。青简词表按当前目标语种导入，保留前两条译词，不导入词性和读音。');
    } catch (error) {
      status(error instanceof Error ? error.message : '文件读取失败');
    } finally { fileInput.value = ''; }
  });
  document.getElementById('personalGlossaryExport')?.addEventListener('click', () => {
    const error = validatePersonalGlossary(field.value);
    if (error) { status(error); return; }
    const url = URL.createObjectURL(new Blob([field.value], { type: 'text/tab-separated-values;charset=utf-8' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'personal-translations.tsv';
    link.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    status('已导出当前词表草稿。');
  });
}
