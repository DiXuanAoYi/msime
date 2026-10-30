import { expect, it } from 'vitest';
import { importQingjianGlossary, validatePersonalGlossary } from './personal-glossary';

it('accepts language-scoped unicode rows, BOM, comments and empty tables', () => {
  expect(validatePersonalGlossary('\uFEFF# personal\r\nen\t开发\tdevelopment\tdevelop\r\nja\t开发\t開発')).toBeNull();
  expect(validatePersonalGlossary('')).toBeNull();
  expect(validatePersonalGlossary('th\t你好\tสวัสดี\nit\t你好\tciao\nvi\t你好\txin chào')).toBeNull();
});

it('reports the offending row without accepting malformed or oversized data', () => {
  expect(validatePersonalGlossary('en\t苹果\tapple\nja\t猫')).toContain('第 2 行');
  for (const bad of ['xx\t猫\tcat', 'en\t\tcat', 'en\t猫\t', 'en\t猫\tcat\t', 'en\t猫\tcat\ta\tb', 'en\t猫\tc\0at']) {
    expect(validatePersonalGlossary(bad)).not.toBeNull();
  }
  expect(validatePersonalGlossary('en\t猫\t' + '猫'.repeat(342))).not.toBeNull();
  expect(validatePersonalGlossary('a'.repeat(65537))).not.toBeNull();
});

it('imports Qingjian senses without committing part-of-speech or reading annotations', () => {
  expect(importQingjianGlossary('# sample\n开发\tv. 開発する|かいはつする\tn. 開発|かいはつ\tthird\n', 'ja'))
    .toBe('ja\t开发\t開発する\t開発; third');
  expect(importQingjianGlossary('架构\tn. architecture', 'en')).toBe('en\t架构\tarchitecture');
  expect(importQingjianGlossary('你好\tphr. hello\tint. hi', 'en')).toBe('en\t你好\thello\thi');
  expect(() => importQingjianGlossary('坏行', 'en')).toThrow('第 1 行');
  expect(() => importQingjianGlossary('猫\tcat', '../en')).toThrow();
});
it('preserves all senses in an exportable legacy table and rejects oversized joined fields',()=>{
  const input='你好\tint. hello\tint. hi\tphr. how do you do\twelcome\tgreetings\tsalutations\tgood day';
  const imported=importQingjianGlossary(input,'en');
  expect(imported).toBe('en\t你好\thello\thi; how do you do; welcome; greetings; salutations; good day');
  expect(validatePersonalGlossary(imported)).toBeNull();
  expect(imported.split('\t')).toHaveLength(4);
  expect(()=>importQingjianGlossary('词\tone\t'+('x'.repeat(600))+'\t'+('y'.repeat(600)),'en')).toThrow();
});
