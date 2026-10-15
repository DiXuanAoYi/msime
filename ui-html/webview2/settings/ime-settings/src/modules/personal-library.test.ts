import { expect, it, vi } from 'vitest';
vi.mock('../utils/host-messages',()=>({onHostMessage:vi.fn()}));
import { filterLibraryEntries } from './personal-library';

it('filters an unpaged clipboard without truncating its tail',()=>{
  const history=Array.from({length:50},(_,i)=>({id:String(i),name:`记录 ${i}`,text:`内容 ${i}`}));
  expect(filterLibraryEntries(history,'')).toHaveLength(50);
  expect(filterLibraryEntries(history,'内容 49').map(x=>x.id)).toEqual(['49']);
});
it('searches all library fields without pagination or altering source data',()=>{
  const entries=Array.from({length:38},(_,i)=>({id:String(i),name:`短语 ${i}`,text:i===27?'特别的正文':'普通正文',application:i===30?'code.exe':'wechat.exe',code:`trigger${i}`}));
  expect(filterLibraryEntries(entries,'')).toHaveLength(38);
  expect(filterLibraryEntries(entries,'').at(-1)?.id).toBe('37');
  expect(filterLibraryEntries(entries,'特别').map(x=>x.id)).toEqual(['27']);
  expect(filterLibraryEntries(entries,'CODE.EXE').map(x=>x.id)).toEqual(['30']);
  expect(filterLibraryEntries(entries,'missing')).toEqual([]);
  expect(entries).toHaveLength(38);
});
