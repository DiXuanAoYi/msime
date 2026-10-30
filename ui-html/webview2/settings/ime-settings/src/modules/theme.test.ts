import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyThemeConfig, reapplyThemeConfig, setThemeMode, setSurfaceTheme, setCandidateSurfaceThemeListener } from './theme';

class FakeImage {
  src = '/assets/softkbd.png';
}

function fakeElement() {
  return {
    style: {} as Record<string, string>,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() }
  };
}

let previewLoaded = false;
let image: FakeImage;
const media = {matches: true, onchange: null as (() => void) | null};
let root: ReturnType<typeof fakeElement>;
let status: {textContent: string; dataset: Record<string, string>};
let follow: {textContent: string; disabled: boolean};
let labels: Map<string, {textContent: string}>;

beforeEach(() => {
  previewLoaded = false;
  image = new FakeImage();
  vi.stubGlobal('HTMLImageElement', FakeImage);
  media.matches = true;
  media.onchange = null;
  root = fakeElement();
  status = {textContent: '', dataset: {}};
  follow = {textContent: '', disabled: true};
  labels = new Map(['themeBtn', 'settingsThemeBtn', 'candThemeBtn', 'ftbThemeBtn', 'voiceThemeBtn'].map(id => [`#${id} span`, {textContent: ''}]));
  vi.stubGlobal('window', { matchMedia: () => media, dispatchEvent: vi.fn() });
  vi.stubGlobal('document', {
    documentElement: root,
    body: fakeElement(),
    querySelector: (selector: string) =>
      selector === '.screenkb-preview' && previewLoaded ? fakeElement() : labels.get(selector) ?? null,
    querySelectorAll: () => [],
    getElementById: (id: string) => id === 'settingsThemeStatus' ? status : id === 'settingsThemeFollowBtn' ? follow : (id === 'screenKeyboardPreviewImage' && previewLoaded ? image : null)
  });
});

it('explains a fixed light override and follows only the settings surface on request', () => {
  applyThemeConfig({theme_mode: 'light', theme_settings: 'light', theme_cand: 'dark', theme_ftb: 'light', theme_voice: 'dark'});
  setThemeMode('dark');
  expect(root.style.colorScheme).toBe('light');
  expect(status.textContent).toContain('修改主主题不会改变本页');
  expect(status.dataset.independent).toBe('true');
  expect(follow.disabled).toBe(false);
  setSurfaceTheme('settings', 'follow');
  expect(root.style.colorScheme).toBe('dark');
  expect(follow.disabled).toBe(true);
  expect(labels.get('#candThemeBtn span')?.textContent).toBe('深色');
  expect(labels.get('#ftbThemeBtn span')?.textContent).toBe('浅色');
  expect(labels.get('#voiceThemeBtn span')?.textContent).toBe('深色');
  setThemeMode('light');
  expect(root.style.colorScheme).toBe('light');
  expect(labels.get('#candThemeBtn span')?.textContent).toBe('深色');
});

it('uses snapshot state for global changes even when all independent controls are unloaded', () => {
  applyThemeConfig({theme_mode: 'light', theme_settings: 'dark', theme_cand: 'light', theme_ftb: 'dark'});
  labels.clear();
  setThemeMode('system');
  expect(root.style.colorScheme).toBe('dark');
  labels.set('#candThemeBtn span', {textContent: ''});
  labels.set('#ftbThemeBtn span', {textContent: ''});
  reapplyThemeConfig();
  expect(labels.get('#candThemeBtn span')?.textContent).toBe('浅色');
  expect(labels.get('#ftbThemeBtn span')?.textContent).toBe('深色');
});

it('follows hot system changes and accepts authoritative native rollback snapshots', () => {
  applyThemeConfig({theme_mode: 'system', theme_settings: 'follow'});
  expect(root.style.colorScheme).toBe('light');
  media.matches = false;
  media.onchange?.();
  expect(root.style.colorScheme).toBe('dark');
  setThemeMode('light');
  expect(root.style.colorScheme).toBe('light');
  applyThemeConfig({theme_mode: 'dark', theme_settings: 'follow'});
  expect(root.style.colorScheme).toBe('dark');
  expect(labels.get('#themeBtn span')?.textContent).toBe('深色');
});

afterEach(() => {
  setCandidateSurfaceThemeListener(null);
  vi.unstubAllGlobals();
});

it('themes a preview page that loads after the snapshot was applied', async () => {
  applyThemeConfig({ theme_mode: 'system', theme_screen_keyboard: 'follow' });
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd.png');

  previewLoaded = true;
  reapplyThemeConfig();
  await vi.dynamicImportSettled();
  expect(image.src).toBe('/assets/softkbd_light.png');
});

it('isolates a broken candidate preview and continues the full theme snapshot', () => {
  setCandidateSurfaceThemeListener(() => {throw new Error('SYNTHETIC_PRIVATE_VALUE_MUST_NOT_LEAK');});
  expect(() => applyThemeConfig({theme_mode: 'dark', theme_settings: 'follow', theme_ftb: 'light'})).not.toThrow();
  expect(root.style.colorScheme).toBe('dark');
  expect(labels.get('#ftbThemeBtn span')?.textContent).toBe('浅色');
  expect(labels.get('#themeBtn span')?.textContent).toBe('深色');
  expect(status.textContent).toContain('设置页跟随主主题');
  const event = vi.mocked(window.dispatchEvent).mock.calls[0][0] as CustomEvent;
  expect(event.type).toBe('settings-handler-error');
  expect(event.detail).toEqual({type: 'configSnapshot'});
  expect(JSON.stringify(event.detail)).not.toContain('SYNTHETIC_PRIVATE_VALUE');
});
