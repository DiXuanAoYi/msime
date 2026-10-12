import { expect, it, vi } from 'vitest';
vi.mock('../utils/host-messages',()=>({onHostMessage:vi.fn()}));
import { libraryPage } from './personal-library';
it('searches the actual library fields and bounds pagination without altering source data',()=>{
  const entries=Array.from({length:38},(_,i)=>({id:String(i),name:`短语 ${i}`,text:i===27?'特别的正文':'普通正文',application:i===30?'code.exe':'wechat.exe',code:`trigger${i}`}));
  expect(libraryPage(entries,'',1).items[0].id).toBe('12');
  expect(libraryPage(entries,'',99)).toMatchObject({page:3,total:38,pages:4});
  expect(libraryPage(entries,'特别',0).items.map(x=>x.id)).toEqual(['27']);
  expect(libraryPage(entries,'CODE.EXE',0).items.map(x=>x.id)).toEqual(['30']);
  expect(libraryPage(entries,'missing',3)).toMatchObject({items:[],page:0,pages:1,total:0});
  expect(libraryPage(entries,'',NaN,-1).items).toHaveLength(12);
  expect(entries).toHaveLength(38);
});
