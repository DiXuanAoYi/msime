let candidateSurfaceThemeListener: ((theme: ResolvedTheme) => void) | null = null;

export function setCandidateSurfaceThemeListener(listener: ((theme: ResolvedTheme) => void) | null): void {
  candidateSurfaceThemeListener = listener;
}

export type ThemeMode = 'dark' | 'light' | 'system';
export type SurfaceTheme = 'follow' | 'dark' | 'light';
export type ResolvedTheme = 'dark' | 'light';

export type ThemeConfig = {
  theme_mode?: string;
  theme_settings?: string;
  theme_cand?: string;
  theme_ftb?: string;
  theme_menu?: string;
  theme_emoji?: string;
  theme_screen_keyboard?: string;
  theme_handwriting?: string;
  theme_voice?: string;
};

const THEME_MODE_LABELS: Record<ThemeMode, string> = {
  dark: '深色',
  light: '浅色',
  system: '跟随系统'
};

const SURFACE_THEME_LABELS: Record<SurfaceTheme, string> = {
  follow: '跟随主主题',
  dark: '深色',
  light: '浅色'
};

let mediaQuery: MediaQueryList | null = null;
let lastThemeConfig: ThemeConfig | null = null;

function normalizeThemeMode(value: string | undefined): ThemeMode {
  if (value === 'light' || value === 'dark' || value === 'system') {
    return value;
  }
  if (value === 'auto') {
    return 'system';
  }
  return 'dark';
}

function normalizeSurfaceTheme(value: string | undefined): SurfaceTheme {
  if (value === 'light' || value === 'dark' || value === 'follow') {
    return value;
  }
  return 'follow';
}

export function isSystemLightTheme(): boolean {
  return window.matchMedia?.('(prefers-color-scheme: light)').matches ?? false;
}

export function resolveTheme(mode: ThemeMode, surface: SurfaceTheme = 'follow'): ResolvedTheme {
  if (surface === 'dark' || surface === 'light') {
    return surface;
  }
  if (mode === 'system') {
    return isSystemLightTheme() ? 'light' : 'dark';
  }
  return mode;
}

export function applyDocumentTheme(theme: ResolvedTheme): void {
  const root = document.documentElement;
  root.setAttribute('data-theme', theme);
  root.style.colorScheme = theme;

  const meta = document.querySelector('meta[name="color-scheme"]');
  if (meta) {
    meta.setAttribute('content', theme);
  }

  document.body?.setAttribute('data-theme', theme);
  document.body?.classList.toggle('theme-light', theme === 'light');
  document.body?.classList.toggle('theme-dark', theme === 'dark');
}

export function applyCandidatePreviewTheme(theme: ResolvedTheme): void {
  document.querySelectorAll('.cand-preview .candidate').forEach((element) => {
    element.classList.toggle('theme-light', theme === 'light');
    element.classList.toggle('theme-dark', theme === 'dark');
  });
  try {
    candidateSurfaceThemeListener?.(theme);
  } catch {
    // A broken optional preview cannot stop the shell or the remaining modules
    // from receiving their snapshot. Never expose an exception/configuration.
    window.dispatchEvent?.(new CustomEvent('settings-handler-error', {detail: {type: 'configSnapshot'}}));
  }
}

function ensureSystemThemeListener(onChange: () => void): void {
  if (!window.matchMedia) {
    return;
  }
  if (!mediaQuery) {
    mediaQuery = window.matchMedia('(prefers-color-scheme: light)');
  }
  mediaQuery.onchange = () => onChange();
}

export function applyThemeConfig(config: ThemeConfig | undefined): void {
  lastThemeConfig = config ? {...config} : null;
  const mode = normalizeThemeMode(config?.theme_mode);
  const settingsTheme = normalizeSurfaceTheme(config?.theme_settings);
  const candTheme = normalizeSurfaceTheme(config?.theme_cand);

  const settingsResolved = resolveTheme(mode, settingsTheme);
  const candResolved = resolveTheme(mode, candTheme);

  applyDocumentTheme(settingsResolved);
  applyCandidatePreviewTheme(candResolved);
  applyFtbPreviewTheme(resolveTheme(mode, normalizeSurfaceTheme(config?.theme_ftb)));
  if (document.querySelector('.screenkb-preview')) {
    const screenKeyboardTheme = resolveTheme(mode, normalizeSurfaceTheme(config?.theme_screen_keyboard));
    void import('./screenkb-settings').then((module) => {
      module.applyScreenKeyboardPreviewTheme(screenKeyboardTheme);
    });
  }
  if (document.querySelector('.handwriting-preview')) {
    const handwritingTheme = resolveTheme(mode, normalizeSurfaceTheme(config?.theme_handwriting));
    void import('./handwriting-settings').then((module) => {
      module.applyHandwritingPreviewTheme(handwritingTheme);
    });
  }

  applyDropdownLabel('themeBtn', THEME_MODE_LABELS[mode], mode);
  applyDropdownLabel('settingsThemeBtn', SURFACE_THEME_LABELS[settingsTheme], settingsTheme);
  applyDropdownLabel('candThemeBtn', SURFACE_THEME_LABELS[candTheme], candTheme);
  applyDropdownLabel('ftbThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_ftb)], normalizeSurfaceTheme(config?.theme_ftb));
  applyDropdownLabel('menuThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_menu)], normalizeSurfaceTheme(config?.theme_menu));
  applyDropdownLabel('emojiThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_emoji)], normalizeSurfaceTheme(config?.theme_emoji));
  applyDropdownLabel('screenKeyboardThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_screen_keyboard)], normalizeSurfaceTheme(config?.theme_screen_keyboard));
  applyDropdownLabel('handwritingThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_handwriting)], normalizeSurfaceTheme(config?.theme_handwriting));
  applyDropdownLabel('voiceThemeBtn', SURFACE_THEME_LABELS[normalizeSurfaceTheme(config?.theme_voice)], normalizeSurfaceTheme(config?.theme_voice));

  ensureSystemThemeListener(() => {
    const modeNow = normalizeThemeMode(config?.theme_mode);
    const settingsNow = normalizeSurfaceTheme(config?.theme_settings);
    const candNow = normalizeSurfaceTheme(config?.theme_cand);
    const ftbNow = normalizeSurfaceTheme(config?.theme_ftb);
    const menuNow = normalizeSurfaceTheme(config?.theme_menu);
    const emojiNow = normalizeSurfaceTheme(config?.theme_emoji);
    const screenKeyboardNow = normalizeSurfaceTheme(config?.theme_screen_keyboard);
    const handwritingNow = normalizeSurfaceTheme(config?.theme_handwriting);
    const voiceNow = normalizeSurfaceTheme(config?.theme_voice);
    if (modeNow !== 'system' &&
        settingsNow !== 'follow' &&
        candNow !== 'follow' &&
        ftbNow !== 'follow' &&
        menuNow !== 'follow' &&
        emojiNow !== 'follow' &&
        screenKeyboardNow !== 'follow' &&
        handwritingNow !== 'follow' &&
        voiceNow !== 'follow') {
      return;
    }
    applyThemeConfig(config);
  });

  updateSettingsThemeState(mode, settingsTheme, settingsResolved);
  updateSkinThemeCard(mode, settingsResolved, candResolved);
}

// Skin, screen keyboard and handwriting pages load after the first snapshot has been applied, so their previews only exist once the theme has already been resolved; they call this when their DOM is ready.
export function reapplyThemeConfig(): void {
  if (lastThemeConfig) {
    applyThemeConfig(lastThemeConfig);
  }
}

export function applyFtbPreviewTheme(theme: ResolvedTheme): void {
  document.querySelectorAll('.ftb-preview-host:not(#skinToolbarPreview):not([data-skin-toolbar])').forEach((element) => {
    element.classList.toggle('theme-light', theme === 'light');
    element.classList.toggle('theme-dark', theme === 'dark');
  });
}

function applyDropdownLabel(btnId: string, label: string, value: string): void {
  const span = document.querySelector<HTMLElement>(`#${btnId} span`);
  const menuId = btnId.replace(/Btn$/, 'Menu');
  document.querySelectorAll<HTMLElement>(`#${menuId} .dropdown-item`).forEach(item => {
    const active = item.dataset.value === value;
    item.classList.toggle('active', active);
    item.setAttribute('aria-selected', String(active));
  });
  if (span) {
    span.textContent = label;
  }
}

function updateSkinThemeCard(
  mode: ThemeMode,
  settingsResolved: ResolvedTheme,
  candResolved: ResolvedTheme
): void {
  const description = document.getElementById('skinThemeStatus');
  if (description) {
    const modeLabel =
      mode === 'system' ? '跟随系统' : mode === 'light' ? '浅色' : '深色';
    description.textContent =
      `全局 ${modeLabel} · 设置 ${settingsResolved === 'light' ? '浅色' : '深色'} · 候选 ${candResolved === 'light' ? '浅色' : '深色'}`;
  }

  // Fluent preview follows the same resolved theme as the real candidate surface.
  if (document.querySelector('[data-skin-switch]')) {
    void import('./skin').then((module) => {
      module.syncSkinPreviewTheme(candResolved);
    });
  }
}

export function setThemeMode(value: string | undefined): void {
  // Native snapshots own all independent overrides, including controls on pages
  // that have not loaded yet. A global change must never infer them from labels.
  applyThemeConfig({...lastThemeConfig, theme_mode: normalizeThemeMode(value)});
}

export function setSurfaceTheme(
  surface: 'settings' | 'cand' | 'ftb' | 'menu' | 'emoji' | 'screenKeyboard' | 'handwriting' | 'voice',
  value: string | undefined
): void {
  const keys = {
    settings: 'theme_settings', cand: 'theme_cand', ftb: 'theme_ftb', menu: 'theme_menu',
    emoji: 'theme_emoji', screenKeyboard: 'theme_screen_keyboard', handwriting: 'theme_handwriting', voice: 'theme_voice'
  } as const;
  applyThemeConfig({...lastThemeConfig, [keys[surface]]: normalizeSurfaceTheme(value)});
}

function updateSettingsThemeState(mode: ThemeMode, surface: SurfaceTheme, resolved: ResolvedTheme): void {
  const status = document.getElementById('settingsThemeStatus');
  const independent = surface !== 'follow';
  if (status) {
    const global = THEME_MODE_LABELS[mode];
    const shown = resolved === 'light' ? '浅色' : '深色';
    status.textContent = independent
      ? `设置页独立使用${shown}，修改主主题不会改变本页。可以一键改为跟随主主题；其他界面的独立设置会保留。`
      : `设置页跟随主主题（${global}），当前显示${shown}。更改主主题会立即更新本页。`;
    status.dataset.independent = String(independent);
  }
  const follow = document.getElementById('settingsThemeFollowBtn') as HTMLButtonElement | null;
  if (follow) {
    follow.disabled = !independent;
    follow.textContent = independent ? '设置页跟随主主题' : '已跟随主主题';
  }
}
