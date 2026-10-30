import {afterEach, expect, it, vi} from 'vitest';
import {applySettingsEnvironment} from './settings-environment';
afterEach(() => vi.unstubAllGlobals());
it('shows a dedicated test notice only for explicit native boolean metadata', () => {
  const notice = {hidden: true};
  vi.stubGlobal('document', {getElementById: () => notice});
  applySettingsEnvironment({temporary_configuration: true});
  expect(notice.hidden).toBe(false);
  for (const value of [undefined, null, {}, {temporary_configuration: false}, {temporary_configuration: 'true'}, {path: 'fixture'}]) {
    applySettingsEnvironment(value);
    expect(notice.hidden).toBe(true);
  }
});
