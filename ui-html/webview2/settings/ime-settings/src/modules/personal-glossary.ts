// Personal translations are stored through the existing atomic config writer.
// Validation here provides useful row errors; the native parser is authoritative.
export const MAX_GLOSSARY_BYTES = 65536;
const languages = new Set(['en', 'ja', 'es', 'fr', 'de', 'ru', 'ko', 'vi', 'th', 'it']);
const bytes = (text: string) => new TextEncoder().encode(text).length;

export function validatePersonalGlossary(text: string): string | null {
  if (bytes(text) > MAX_GLOSSARY_BYTES) return '词表不能超过 64 KB，请分批整理后再导入。';
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/u.test(text)) return '词表包含不支持的控制字符。';
  const lines = text.replace(/^\uFEFF/, '').split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]!.replace(/^[ \r]+|[ \r]+$/g, '');
    if (!line || line.startsWith('#')) continue;
    const fields = line.split('\t').map(value => value.replace(/^[ \r]+|[ \r]+$/g, ''));
    if (fields.length < 3 || fields.length > 4 || !languages.has(fields[0]!) ||
        fields.some(value => !value || bytes(value) > 1024)) {
      return `第 ${i + 1} 行格式有误：请用 Tab 分隔语言、中文词、译词（可加第二释义）。支持 en / ja / es / fr / de / ru / ko / vi / th / it。`;
    }
  }
  return null;
}

// Per-language TSV contains a word and one or more sense columns, optionally annotated
// with POS/readings. Preserve every translation; fold extra senses into the legacy fourth
// column so old three/four-column multilingual tables can still be imported and exported.
export function importQingjianGlossary(text: string, language: string): string {
  if (!languages.has(language)) throw new Error('请选择有效的目标语种。');
  if (bytes(text) > MAX_GLOSSARY_BYTES) throw new Error('导入文件不能超过 64 KB。');
  const result: string[] = [];
  for (const [index, raw] of text.replace(/^\uFEFF/, '').split(/\r?\n/).entries()) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const [word, ...rawSenses] = line.split('\t').map(value => value.trim());
    const senses = rawSenses.filter(Boolean).map(value =>
      value.split('|')[0]!.replace(/^(?:n|noun|v|verb|adj|adv|pron|prep|conj|num|m|mw|part|int|interj|phr|phrase)\.\s+/iu, '').trim()
    );
    if (!word || !senses.length || senses.some(value => !value)) throw new Error(`第 ${index + 1} 行缺少词或译词。`);
    result.push([language, word, senses[0]!, ...(senses.length>1?[senses.slice(1).join('; ')]:[])].join('\t'));
  }
  const converted = result.join('\n');
  const error = validatePersonalGlossary(converted);
  if (error) throw new Error(error);
  return converted;
}
