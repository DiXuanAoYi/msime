import {expect,it,vi} from 'vitest';
vi.mock('./config-sync',()=>({updateConfig:vi.fn()}));
vi.mock('./shared',()=>({applyToggleState:vi.fn(),setupToggleButton:vi.fn()}));
import {parseEnglishRules} from './input-advanced';
import {modelSize} from './neural-models';
it('retains explicit app overrides and removes invalid duplicate rules',()=>{expect(parseEnglishRules('[{"application":"Code.exe","mode":"disabled"},{"application":"code.exe","mode":"enabled"},{"application":"QQ.exe","mode":"inherit"},{"application":"bad.exe","mode":"unknown"}]')).toEqual([{application:'code.exe',mode:'disabled'},{application:'qq.exe',mode:'inherit'}]);expect(parseEnglishRules('{')).toEqual([]);});
it('distinguishes actual installed model size from a package size',()=>{expect(modelSize(25480184)).toContain('24.30 MiB');expect(modelSize(143814324)).toContain('137.15 MiB');expect(modelSize(0)).toContain('导入后');});
