let skinInitialized = false;
let catalogKey = '';
import { serializeHostMessage } from '../../../../shared/messages';
import { loadHTML } from '../utils/common-utils';
import { applyToolbarIconGlyphFallbacks } from './toolbar-icon-glyphs';
import ftbHTML from '../../../../ftb/default.html?raw';

export type SkinPreviewTheme = 'dark' | 'light';
export type CandidateSkin = string;

type CandidateColors = {
  accent?: string; selected?: string; hover?: string; surface?: string;
  border?: string; text?: string; number?: string; translation?: string; showSelectedBar?: boolean;
  candidateText?: string; preeditText?: string; preeditCaret?: string; selectedText?: string;
  selectedNumber?: string; selectedTranslation?: string; selectedBar?: string;
  translationPos?:string;translationSeparator?:string;selectedTranslationPos?:string;selectedTranslationSeparator?:string;
  preeditBackground?: string; preeditDivider?: string;
  menu?: { background?: string; border?: string; text?: string; hover?: string };
};
type ToolbarColors = {
  background?: string; border?: string; handle?: string; divider?: string; icon?: string; hover?: string;
};
type ExternalSkin = {
  id: string; name: string; version: string; author?: string; description?: string;
  base: string; layouts: string[]; themes: string[];
  minWidthDip?: number; decorationTopDip?: number; decorationWidthDip?: number; compatible: boolean;
  decorationImage?: string; decorationAlign?: string; cornerRadiusDip?: number | null;
  backgroundImage?: string; backgroundFit?: string; backgroundOpacity?: number;
  candidate?: { dark?: CandidateColors; light?: CandidateColors };
  toolbar?: { dark?: ToolbarColors; light?: ToolbarColors }; toolbarCornerRadiusDip?: number | null;
  borderWidthDip?: number | null; itemCornerRadiusDip?: number | null; shadow?: string; fontFamily?: string;
  // Already resolved host-side, including the fallback to the base skin's skins/default manifest.
  pageArrows?: boolean;fitContent?:boolean;
};
type SkinScanIssue = { folder: string; reason: string };

const BUILTIN_SKINS = ['fluent', 'wechat', 'graphite', 'willow_green', 'autumn_osmanthus', 'microsoft'] as const;
type BuiltinSkin = typeof BUILTIN_SKINS[number];
const previewOverrides: Record<string, SkinPreviewTheme | null> = {
  fluent: null, wechat: null, graphite: null, willow_green: null, autumn_osmanthus: null, microsoft: null
};
const BUILTIN_PREVIEW_CLASSES = ['skin-wechat', 'skin-graphite', 'skin-willow-green', 'skin-autumn-osmanthus', 'skin-microsoft'];
const loadedExternalStyleIds = new Set<string>();
let activeTheme: SkinPreviewTheme = 'dark';
let activeSkin: CandidateSkin = 'fluent';
let externalSkins: ExternalSkin[] = [];
let scanIssues: SkinScanIssue[] = [];
let skinDirectory = '';
let catalogScanned = false;
let catalogRevision = 0;
let previewHorizontalHtml = '';
let previewVerticalHtml = '';
const SKIN_PREVIEW_PAGE_SIZE = 6;
// page_arrows of the built-in skins, read host-side from skins/default/<id>/skin.toml. Off unless a manifest says so.
let builtinPageArrows: Record<string, boolean> = {};
let builtinFitContent:Record<string,boolean>={};
let fitMode: "follow"|"enabled"|"disabled"="follow";
export function applyBuiltinSkinFitContent(raw:unknown):void {if(raw&&typeof raw==="object")builtinFitContent=raw as Record<string,boolean>;syncAppearancePreviews();}
export function applyCandidateFitContentMode(mode:string|undefined):void {fitMode=mode==="enabled"||mode==="disabled"?mode:"follow";syncAppearancePreviews();}
export function skinFitsContent(id:string):boolean {return fitMode==="enabled"||(fitMode==="follow"&&(findExternalSkin(id)?.fitContent??builtinFitContent[id])===true);}

function normalizeCandidateSkin(value: unknown): CandidateSkin {
  return typeof value === 'string' && /^[a-z0-9][a-z0-9._-]{0,63}$/.test(value) ? value : 'fluent';
}

function asBuiltinSkin(value: unknown): BuiltinSkin {
  return BUILTIN_SKINS.includes(value as BuiltinSkin) ? value as BuiltinSkin : 'fluent';
}

function findExternalSkin(id: string): ExternalSkin | undefined {
  return externalSkins.find((skin) => skin.id === id);
}

function builtinPreviewClass(skinId: string): string {
  if (skinId === 'wechat') return 'skin-wechat';
  if (skinId === 'graphite') return 'skin-graphite';
  if (skinId === 'willow_green') return 'skin-willow-green';
  if (skinId === 'autumn_osmanthus') return 'skin-autumn-osmanthus';
  if (skinId === 'microsoft') return 'skin-microsoft';
  const external = findExternalSkin(skinId);
  return external ? builtinPreviewClass(external.base) : '';
}

export function skinShowsPageArrows(skinId: string): boolean {
  const external = findExternalSkin(skinId);
  if (external) return external.pageArrows === true;
  return builtinPageArrows[skinId] === true;
}

function setPreviewPageArrows(host: Element, skinId: string): void {
  host.classList.toggle('page-arrows-on', skinShowsPageArrows(skinId));
}

function syncBuiltinCardPageArrows(): void {
  BUILTIN_SKINS.forEach((skin) => {
    document.querySelectorAll(`[data-candidate-skin="${skin}"] :is([data-skin-horizontal], [data-skin-vertical])`)
      .forEach((host) => setPreviewPageArrows(host, skin));
  });
}

function limitCandidatePreview(host: HTMLElement): void {
  const wrappers = host.querySelectorAll<HTMLElement>('.row-wrapper');
  const lastVisible = Math.min(wrappers.length, SKIN_PREVIEW_PAGE_SIZE) - 1;
  wrappers.forEach((wrapper, index) => {
    wrapper.style.display = index < SKIN_PREVIEW_PAGE_SIZE ? '' : 'none';
    // Mirrors the candidate window's ApplyCandidateFrame: hidden rows keep :last-child.
    wrapper.classList.toggle('last-visible', index === lastVisible);
  });
}

function wrapCandidateHtml(html: string): string {
  return `<div class="containerParent">${html}</div>`;
}

function fillPreviewHost(host: HTMLElement, html: string): void {
  host.innerHTML = wrapCandidateHtml(html);
  host.querySelectorAll('.container').forEach((container) => {
    container.classList.add('hover-active');
  });
  limitCandidatePreview(host);
}

function fillToolbar(host: HTMLElement): void {
  const source = new DOMParser().parseFromString(ftbHTML, 'text/html');
  const statusBar = source.querySelector<HTMLElement>('.status-bar');
  if (!statusBar) return;
  statusBar.querySelectorAll('#en, #fullwidth, #puncEn').forEach((element) => element.remove());
  statusBar.querySelectorAll<HTMLElement>('[id]').forEach((element) => element.removeAttribute('id'));
  applyToolbarIconGlyphFallbacks(statusBar);
  host.replaceChildren(statusBar);
}

function resourceUrl(id: string, relativePath: string): string {
  return `https://candidate-skins.example/${encodeURIComponent(id)}/${relativePath.split('/').map(encodeURIComponent).join('/')}?v=${catalogRevision}`;
}

function applyDecorationVars(host: HTMLElement, skin: ExternalSkin): void {
  host.style.setProperty('--msime-skin-min-width', `${skin.minWidthDip || 0}px`);
  host.style.setProperty('--msime-skin-decoration-top', `${skin.decorationTopDip || 0}px`);
  host.style.setProperty('--msime-skin-decoration-width', `${skin.decorationWidthDip || 0}px`);
}

function clearDecorationVars(host: HTMLElement): void {
  host.style.removeProperty('--msime-skin-min-width');
  host.style.removeProperty('--msime-skin-decoration-top');
  host.style.removeProperty('--msime-skin-decoration-width');
}

// Skin manifests come from an arbitrary folder in the skins directory and only their file names are validated host-side, so a colour string is rejected unless it is one of the plain notations we actually render. Anything else could carry a declaration or brace and rewrite the rule it is pasted into.
const SKIN_COLOR_PATTERN = /^(#[0-9a-f]{3,4}|#[0-9a-f]{6}|#[0-9a-f]{8}|rgb\(\s*\d{1,3}\s*(,|\s)\s*\d{1,3}\s*(,|\s)\s*\d{1,3}\s*\)|rgba\(\s*\d{1,3}\s*(,|\s)\s*\d{1,3}\s*(,|\s)\s*\d{1,3}\s*(,|\/)\s*(0|1|0?\.\d+|\d{1,3}%)\s*\))$/i;

function skinColor(value: string | undefined): string | undefined {
  const trimmed = value?.trim();
  return trimmed && SKIN_COLOR_PATTERN.test(trimmed) ? trimmed : undefined;
}

// Numbers from the manifest are pasted into CSS text, so anything that is not a finite number in range is dropped.
function boundedNumber(value: unknown, max: number): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= max ? value : undefined;
}

// The candidate preview host carries the theme and layout classes. It is the scope root itself on the appearance pages,
// but sits inside the skin card that is the scope root in the skin list, where ":scope.theme-light" never matched and
// the light preview fell back to the dark rules.
const CANDIDATE_HOST = ':is(:scope, :scope .candidate)';

// Mirrors the candidate window's corner override (BuildExternalCandidateSkinCss): the frame, the variables the
// willow green / autumn osmanthus skins read, and the fluent / wechat horizontal highlight corners on the frame.
function cornerPreviewCss(skin: ExternalSkin): string {
  const radius = boundedNumber(skin.cornerRadiusDip, 32);
  if (radius === undefined) return '';
  const r = `${radius}px`;
  // Anchored on the host like the theme rules: the preview's base rules are ".candidate.skin-graphite .container" and
  // friends, which a bare ":scope .container" lost to on specificity, so graphite previews kept 3px against D2D's radius.
  let css = `${CANDIDATE_HOST} .container { border-radius: ${r}; --wg-radius: ${r}; --ao-radius: ${r}; }\n`;
  if (skin.base === 'willow_green') css += `:scope .containerParent { border-radius: ${r}; }\n`;
  if (skin.base === 'fluent' || skin.base === 'wechat') {
    const h = `${CANDIDATE_HOST}.wnd-h .container`;
    css += `${h} > .pinyin + .row-wrapper > .cand { border-bottom-left-radius: ${r}; }
${h}.preedit-hidden > .pinyin + .row-wrapper > .cand { border-top-left-radius: ${r}; }
${h} > .row-wrapper:is(:last-child, .last-visible) > .cand { border-bottom-right-radius: ${r}; }
${h}.preedit-hidden > .row-wrapper:is(:last-child, .last-visible) > .cand { border-top-right-radius: ${r}; }\n`;
  }
  return css;
}

// Mirrors the candidate window: the image covers the whole card including the area under its border, which is drawn on
// top, as in D2D. A ::before cannot reach there because the card is a scroll container that clips to its padding box, so
// the translucent border showed a ring of bare surface. A layer has no opacity of its own, so a veil of the surface at
// (1 - opacity) over the image gives the same result as the image at that opacity over the surface.
function backgroundPreviewCss(skin: ExternalSkin): string {
  if (!skin.backgroundImage) return '';
  const size = skin.backgroundFit === 'contain' ? 'contain' : skin.backgroundFit === 'stretch' ? '100% 100%' : 'cover';
  const opacity = boundedNumber(skin.backgroundOpacity, 1) ?? 1;
  const veilPercent = Math.round((1 - opacity) * 1000) / 10;
  const image = `url("${resourceUrl(skin.id, skin.backgroundImage)}")`;
  const rule = (scope: string, colors: CandidateColors) => {
    // The same surface the card is painted with: the manifest's, else the base preview's variable.
    const surface = skinColor(colors.surface) ?? (skin.base === 'willow_green' ? 'var(--wg-surface)' : 'var(--cand-bg)');
    const veil = `color-mix(in srgb, ${surface} ${veilPercent}%, transparent)`;
    return `${CANDIDATE_HOST}${scope} .container:not(:empty) { background-image: linear-gradient(${veil}, ${veil}), ${image}; background-size: auto, ${size}; background-position: center; background-repeat: no-repeat; background-origin: border-box; background-clip: border-box; }\n`;
  };
  return rule('', skin.candidate?.dark || {}) + rule('.theme-light', skin.candidate?.light || {});
}

function decorationHorizontalCss(align: string | undefined): string {
  if (align === 'left') return 'left: 0;';
  if (align === 'center') return 'left: 0; right: 0; margin-inline: auto;';
  return 'right: 0;';
}

// The base skins' highlight corner, the same values as CandidateSkinBaseItemRadiusDip on the host side.
function baseItemRadius(base: string): number {
  if (base === 'willow_green') return 0;
  if (base === 'graphite') return 2;
  if (base === 'autumn_osmanthus') return 6;
  return 4;
}

// A font name ends up inside a quoted CSS string, so anything that could close the string or the declaration is refused,
// matching the host-side check that rejects the whole skin.
function skinFontFamily(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  const trimmed = value.trim();
  return trimmed && trimmed.length <= 64 && !/[\u0000-\u001f\u007f"'\\,;{}<>`]/.test(trimmed) ? trimmed : undefined;
}

// Mirrors AppendExternalCandidateGeometryCss: preedit band and divider, frame width, highlight corners and card shadow.
function geometryPreviewCss(skin: ExternalSkin, colors: CandidateColors, prefix: string): string {
  const light = prefix.includes('.theme-light');
  const itemRadius = boundedNumber(skin.itemCornerRadiusDip, 16);
  const borderWidth = boundedNumber(skin.borderWidthDip, 4);
  const preeditBackground = skinColor(colors.preeditBackground);
  const preeditDivider = skinColor(colors.preeditDivider);
  let css = '';
  if (preeditBackground) {
    css += `${prefix}.row.pinyin { background: ${preeditBackground}; border-radius: ${itemRadius ?? baseItemRadius(skin.base)}px; }\n`;
  }
  if (preeditDivider) css += `${prefix}.row.pinyin { border-bottom: 1px solid ${preeditDivider}; }\n`;
  if (borderWidth !== undefined) {
    // Willow green and autumn osmanthus draw no frame (border: none), so they need a style and, without a manifest
    // border colour, the transparent frame their palette resolves to.
    const noFrame = skin.base === 'willow_green' || skin.base === 'autumn_osmanthus';
    const color = skinColor(colors.border) ?? (noFrame ? 'transparent' : undefined);
    css += `${prefix}.container:not(:empty) { border-width: ${borderWidth}px; border-style: solid;${color ? ` border-color: ${color};` : ''} }\n`;
  }
  if (itemRadius !== undefined) {
    css += `${prefix}.container { --ao-item-radius: ${itemRadius}px; }\n`;
    css += `${prefix}.container .cand, ${prefix}.container .cand.first { border-radius: ${itemRadius}px; }\n`;
  }
  if (skin.shadow === 'none' || skin.shadow === 'soft' || skin.shadow === 'strong') {
    const target = skin.base === 'willow_green' ? '.containerParent:not(:empty)' : '.container:not(:empty)';
    let value = 'none';
    if (skin.shadow !== 'none') {
      const scale = skin.shadow === 'soft' ? 0.5 : 1.6;
      const alpha = (base: number) => Math.min(base * scale, 1).toFixed(3);
      value = `8px 10px 24px rgba(0, 0, 0, ${alpha(light ? 0.18 : 0.34)}), 2px 3px 8px rgba(0, 0, 0, ${alpha(light ? 0.10 : 0.22)})`;
    }
    css += `${prefix}${target} { box-shadow: ${value}; }\n`;
  }
  return css;
}

// The skin font goes in front of the user's fonts, which stay as fallbacks for missing glyphs. The preview's own rules
// pin the font with !important, so this one has to as well.
function fontPreviewCss(skin: ExternalSkin): string {
  const family = skinFontFamily(skin.fontFamily);
  if (!family) return '';
  const value = `"${family}", var(--cand-font-family, inherit)`;
  // Not CANDIDATE_HOST for the host itself: in the skin list the scope root is the whole card, header included.
  const host = ':is(:scope.candidate, :scope .candidate)';
  return `${host}, ${host} :is(.container, .text, .cand-content, .cand-helpcode, .num, .cand-no, .pinyin) { font-family: ${value} !important; }\n`;
}

function candidatePreviewCss(skin: ExternalSkin): string {
  const dark = skin.candidate?.dark || {};
  const light = skin.candidate?.light || {};
  const themeRules = (scope: string, colors: CandidateColors) => {
    // Inside @scope a selector without :scope only matches below the preview host, so ".theme-light .first" never saw the host's own theme class, and bare ".first::before" lost on specificity to the preview's ".wnd-h .first::before". Anchoring on the host fixes both.
    const prefix = `${CANDIDATE_HOST}${scope} `;
    const accent = skinColor(colors.accent);
    const selected = skinColor(colors.selected);
    const hover = skinColor(colors.hover);
    const surface = skinColor(colors.surface);
    const border = skinColor(colors.border);
    const text = skinColor(colors.text);
    const number = skinColor(colors.number);
    const translation = skinColor(colors.translation);
    let css = '';
    const variables = [surface && `--cand-bg: ${surface}`, border && `--cand-border: ${border}`,
      text && `--cand-text: ${text}`].filter(Boolean).join('; ');
    if (variables) {
      const theme = scope ? 'light' : 'dark';
      css += `:is(:scope.candidate, :scope .candidate).theme-${theme} { ${variables}; }\n`;
      css += `:scope.caret-state-preview-host.theme-${theme} { ${variables}; }\n`;
    }
    if (accent) css += `${prefix}.cursor, ${prefix}.first::before { background: ${accent}; }\n`;
    if (selected) css += `${prefix}.first, ${prefix}.cand.first, ${prefix}.cand.first:hover { background-color: ${selected}; }\n`;
    if (hover) css += `${prefix}.cand:not(.first):hover { background-color: ${hover} !important; }\n`;
    if (surface) css += `${prefix}.container { background: ${surface}; }\n`;
    if (border) css += `${prefix}.container { border-color: ${border}; }\n`;
    if (text) css += `${prefix}.container { color: ${text}; }\n`;
    if (number) css += `${prefix}.num, ${prefix}.cand-no { color: ${number}; }\n`;
    // Mirrors the candidate window: an explicit translation colour is used as-is instead of the inherited text colour at opacity .62.
    if (translation) css += `${prefix}.cand-translation { color: ${translation}; opacity: 1; }\n`;
    if (colors.showSelectedBar === false) css += `${prefix}.first::before { display: none; }\n`;
    // Detailed colours, mirroring AppendExternalCandidateColorCss. The user's text colour (--msime-user-text) still wins
    // over candidate_text / preedit_text, and :where(.cand) keeps the base skin's selected-row text colour in force.
    const candidateText = skinColor(colors.candidateText);
    const preeditText = skinColor(colors.preeditText);
    const preeditCaret = skinColor(colors.preeditCaret);
    const selectedBar = skinColor(colors.selectedBar);
    const selectedText = skinColor(colors.selectedText);
    const selectedNumber = skinColor(colors.selectedNumber);
    const selectedTranslation = skinColor(colors.selectedTranslation);
    if (candidateText) css += `${prefix}:where(.cand) .text { color: var(--msime-user-text, ${candidateText}); }\n`;
    if (preeditText) css += `${prefix}.pinyin .text { color: var(--msime-user-text, ${preeditText}); }\n`;
    if (preeditCaret) css += `${prefix}.cursor { background: ${preeditCaret}; }\n`;
    if (selectedBar) css += `${prefix}.first::before { background: ${selectedBar}; }\n`;
    if (selectedText) css += `${prefix}.cand.first .text { color: ${selectedText}; }\n`;
    if (selectedNumber) css += `${prefix}.cand.first .num, ${prefix}.cand.first .cand-no { color: ${selectedNumber}; }\n`;
    if (selectedTranslation) {
      css += `${prefix}.cand.first .cand-translation { color: ${selectedTranslation}; opacity: 1; }\n`;
    }
    for (const [key, selector] of [['translationPos','.cand-translation .cand-pos'],['translationSeparator','.cand-translation .cand-sep'],['selectedTranslationPos','.cand.first .cand-translation .cand-pos'],['selectedTranslationSeparator','.cand.first .cand-translation .cand-sep']] as const) {
      const color=skinColor(colors[key]);if(color)css+=`${prefix}${selector} {color:${color};opacity:1;}\n`;
    }
    css += geometryPreviewCss(skin, colors, prefix);
    return css;
  };
  // Same geometry as the candidate window: the card is at least as wide as the decoration, and the decoration box
  // sits on top of the card without overlapping it, aligned to the card's left, centre or right edge.
  let css = `.container:not(:empty) { min-width: max(7em, var(--msime-skin-min-width, 0px), var(--msime-skin-decoration-width, 0px)); }\n`;
  if (skin.decorationImage && (skin.decorationTopDip || 0) > 0) {
    const decoration = `url("${resourceUrl(skin.id, skin.decorationImage)}")`;
    css += `.containerParent { padding-top: var(--msime-skin-decoration-top, 0px); position: relative; box-sizing: border-box; }
.containerParent:not(:empty)::before {
  content: ""; position: absolute; z-index: 0; top: 0; ${decorationHorizontalCss(skin.decorationAlign)}
  width: var(--msime-skin-decoration-width, 0px); height: var(--msime-skin-decoration-top, 0px);
  background: ${decoration} center / contain no-repeat; pointer-events: none;
}
.container { position: relative; z-index: 1; }\n`;
  }
  css += cornerPreviewCss(skin);
  css += themeRules('', dark);
  css += themeRules('.theme-light', light);
  // After the theme rules: their `background` shorthand for the surface would otherwise reset the image layers.
  css += backgroundPreviewCss(skin);
  css += fontPreviewCss(skin);
  css += toolbarPreviewCss(skin);
  return css;
}

// Mirrors the floating toolbar's [toolbar] overrides (BuildExternalToolbarSkinCss). The toolbar host is either the scope
// root itself (appearance pages) or sits inside the skin card that is the scope root, and each theme is keyed on the
// host's own theme class so dark values never leak into the light preview. Hover is not previewed: the host ignores
// pointer events.
function toolbarPreviewCss(skin: ExternalSkin): string {
  let css = '';
  const radius = boundedNumber(skin.toolbarCornerRadiusDip, 32);
  (['dark', 'light'] as const).forEach((theme) => {
    const colors = skin.toolbar?.[theme] || {};
    const host = `:is(:scope.ftb-preview-host, :scope .ftb-preview-host).theme-${theme}`;
    const background = skinColor(colors.background);
    const border = skinColor(colors.border);
    const handle = skinColor(colors.handle);
    const divider = skinColor(colors.divider);
    const icon = skinColor(colors.icon);
    if (background) css += `${host} .status-bar { background-color: ${background}; }\n`;
    if (border) css += `${host} .status-bar { border-color: ${border}; }\n`;
    if (radius !== undefined) css += `${host} .status-bar { border-radius: calc(${radius}px * var(--ftb-scale)); }\n`;
    if (handle) css += `${host} .drag-handle { background: ${handle}; }\n`;
    if (divider) css += `${host} .divider { background-color: ${divider}; }\n`;
    if (icon) css += `${host} .icon { color: ${icon}; }\n`;
  });
  return css;
}

// Nesting skin CSS by concatenating it into an "@scope (...) { ... }" string is purely textual, so a stray "}" in the skin closes the block early and everything after it styles the whole settings window instead of just the preview. The scope rule is therefore created empty and every rule parsed out of the skin is re-inserted as a child of it, which nothing can escape from however the braces are balanced.
function writeScopedSkinRules(
  style: HTMLStyleElement, skinId: string, css: string, includeCaretPreview = false
): void {
  const sheet = style.sheet;
  if (!sheet) return;
  while (sheet.cssRules.length) sheet.deleteRule(0);
  const scopeRoot = includeCaretPreview
    ? `:is([data-external-skin-preview="${skinId}"], [data-external-caret-skin-preview="${skinId}"])`
    : `[data-external-skin-preview="${skinId}"]`;
  sheet.insertRule(`@scope (${scopeRoot}) {}`, 0);
  const scope = sheet.cssRules[0] as CSSGroupingRule;
  const parsed = new CSSStyleSheet();
  parsed.replaceSync(css);
  Array.from(parsed.cssRules).forEach((rule) => {
    try {
      scope.insertRule(rule.cssText, scope.cssRules.length);
    } catch {
      // Drop a rule that is not valid inside a grouping rule instead of losing the rest of the skin.
    }
  });
}

function injectGeneratedCandidateCss(skin: ExternalSkin): void {
  const styleId = `external-skin-style-${skin.id}`;
  if (loadedExternalStyleIds.has(styleId) && document.getElementById(styleId)) return;
  const css = candidatePreviewCss(skin);
  if (!css.trim()) return;
  let style = document.getElementById(styleId) as HTMLStyleElement | null;
  if (!style) {
    style = document.createElement('style');
    style.id = styleId;
    style.dataset.externalSkinStyle = skin.id;
    document.head.appendChild(style);
  }
  writeScopedSkinRules(style, skin.id, css, true);
  loadedExternalStyleIds.add(styleId);
}

function resetExternalSkinStyles(): void {
  document.querySelectorAll('style[data-external-skin-style]').forEach((node) => node.remove());
  loadedExternalStyleIds.clear();
}

function resolvedPreviewTheme(skinId: string, supported?: string[]): SkinPreviewTheme {
  const override = previewOverrides[skinId];
  if (override) return override;
  if (!supported || supported.length === 0 || supported.includes(activeTheme)) return activeTheme;
  return supported[0] === 'light' ? 'light' : 'dark';
}

function applyBuiltinCardTheme(skin: BuiltinSkin, theme: SkinPreviewTheme): void {
  const card = document.querySelector<HTMLElement>(`[data-candidate-skin="${skin}"]`);
  if (!card) return;
  card.querySelectorAll<HTMLElement>('[data-skin-horizontal], [data-skin-vertical], [data-skin-toolbar]')
    .forEach((element) => {
      element.classList.toggle('theme-light', theme === 'light');
      element.classList.toggle('theme-dark', theme === 'dark');
    });
  const title = card.querySelector<HTMLElement>('.section-title');
  if (title) {
    const name = skin === 'wechat' ? '微信绿主题' : skin === 'graphite' ? '石墨 Graphite'
      : skin === 'willow_green' ? '杨柳青 Willow green'
      : skin === 'autumn_osmanthus' ? '秋桂 Autumn osmanthus'
      : skin === 'microsoft' ? '微软 Microsoft' : 'Fluent 主题';
    title.textContent = `${name}(${theme === 'light' ? 'Light' : 'Dark'})`;
  }
  card.querySelectorAll<HTMLButtonElement>('[data-skin-preview-switch]').forEach((button) => {
    button.textContent = theme === 'light' ? '预览深色' : '预览浅色';
  });
}

function applyExternalCardTheme(skin: ExternalSkin): void {
  const card = document.querySelector<HTMLElement>(`[data-candidate-skin="${skin.id}"]`);
  if (!card) return;
  const theme = resolvedPreviewTheme(skin.id, skin.themes);
  card.querySelectorAll<HTMLElement>('[data-skin-horizontal], [data-skin-vertical], [data-skin-toolbar]')
    .forEach((element) => {
      element.classList.toggle('theme-light', theme === 'light');
      element.classList.toggle('theme-dark', theme === 'dark');
    });
  card.querySelectorAll<HTMLButtonElement>('[data-skin-preview-switch]').forEach((button) => {
    button.textContent = theme === 'light' ? '预览深色' : '预览浅色';
  });
}

function syncSkinSwitches(): void {
  document.querySelectorAll<HTMLElement>('[data-skin-switch]').forEach((element) => {
    const selected = element.dataset.skinSwitch === activeSkin;
    element.classList.toggle('active', selected);
    element.setAttribute('aria-checked', String(selected));
  });
}

function ensureContainerParent(root: HTMLElement): void {
  const box = root.querySelector<HTMLElement>(':scope > .container, .container');
  if (!box || box.parentElement?.classList.contains('containerParent')) return;
  if (box.parentElement !== root && !root.contains(box)) return;
  const parent = document.createElement('div');
  parent.className = 'containerParent';
  box.replaceWith(parent);
  parent.append(box);
}

export function syncAppearancePreviews(): void {
  const previewClass = builtinPreviewClass(activeSkin);
  const external = findExternalSkin(activeSkin);
  document.querySelectorAll<HTMLElement>('.cand-preview .candidate').forEach((element) => {
    const caretPreview = element.classList.contains('caret-state-preview-host');
    BUILTIN_PREVIEW_CLASSES.forEach((name) => element.classList.toggle(name, previewClass === name));
    if (!caretPreview) {
      ensureContainerParent(element);
      setPreviewPageArrows(element, activeSkin);
      element.dataset.fitContent=skinFitsContent(activeSkin)?"on":"off";
    }
    if (external) {
      if (caretPreview) {
        element.dataset.externalCaretSkinPreview = external.id;
        delete element.dataset.externalSkinPreview;
        clearDecorationVars(element);
      } else {
        element.dataset.externalSkinPreview = external.id;
        delete element.dataset.externalCaretSkinPreview;
        applyDecorationVars(element, external);
      }
    } else {
      delete element.dataset.externalSkinPreview;
      delete element.dataset.externalCaretSkinPreview;
      clearDecorationVars(element);
    }
  });
  document.querySelectorAll<HTMLElement>('.ftb-preview-host:not([data-skin-toolbar])').forEach((element) => {
    BUILTIN_PREVIEW_CLASSES.forEach((name) => element.classList.toggle(name, previewClass === name));
    if (external) {
      element.dataset.externalSkinPreview = external.id;
    } else {
      delete element.dataset.externalSkinPreview;
    }
  });
  document.querySelectorAll<HTMLElement>('.cand-preview').forEach((element) => {
    element.classList.toggle('has-skin-decoration', !!(external && (external.decorationTopDip || 0) > 0));
  });
  if (external) injectGeneratedCandidateCss(external);
}

function selectSkin(value: unknown, persist: boolean): void {
  activeSkin = normalizeCandidateSkin(value);
  syncSkinSwitches();
  syncAppearancePreviews();
  if (persist) {
    window.chrome?.webview?.postMessage(serializeHostMessage({
      type: 'configUpdate', data: { path: 'appearance.candidate_skin', value: activeSkin }
    }));
  }
}

function bindSkinSwitch(element: HTMLElement, skinId: string, enabled: boolean): void {
  element.dataset.skinSwitch = skinId;
  element.setAttribute('role', 'switch');
  element.tabIndex = enabled ? 0 : -1;
  element.setAttribute('aria-disabled', String(!enabled));
  if (!enabled) return;
  const activate = () => selectSkin(skinId, true);
  element.addEventListener('click', activate);
  element.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); activate(); }
  });
}

function renderExternalSkins(): void {
  if (!skinInitialized) return;
  const list = document.getElementById('externalSkinList');
  const empty = document.getElementById('externalSkinEmpty');
  const directory = document.getElementById('externalSkinDirectory');
  const diagnostics = document.getElementById('externalSkinDiagnostics');
  if (!list || !empty || !directory || !diagnostics) return;

  directory.textContent = skinDirectory || '%LOCALAPPDATA%\\metasequoiaime\\skins';
  list.replaceChildren(...externalSkins.map((skin) => {
    const card = document.createElement('div');
    const previewClass = builtinPreviewClass(skin.base);
    card.className = `section skin-theme-card external-skin-live-card${skin.compatible ? '' : ' is-incompatible'}${(skin.decorationTopDip || 0) > 0 ? ' has-skin-decoration' : ''}`;
    card.dataset.candidateSkin = skin.id;
    card.dataset.externalSkinPreview = skin.id;
    applyDecorationVars(card, skin);

    const header = document.createElement('div');
    header.className = 'skin-theme-header';
    const titles = document.createElement('div');
    const title = document.createElement('div');
    title.className = 'section-title';
    title.textContent = skin.name;
    const meta = document.createElement('div');
    meta.className = 'external-skin-meta';
    meta.textContent = [skin.id, skin.version && `v${skin.version}`, skin.author].filter(Boolean).join(' · ');
    const description = document.createElement('div');
    description.className = 'skin-theme-description';
    description.textContent = skin.compatible ? (skin.description || `基于 ${skin.base}`)
      : `当前布局或明暗模式不受支持（${skin.layouts.join('/')}，${skin.themes.join('/')}）`;
    titles.append(title, meta, description);

    const actions = document.createElement('div');
    actions.className = 'skin-theme-actions';
    const toggle = document.createElement('div');
    toggle.className = 'ftb-toggle-btn skin-theme-toggle';
    const knob = document.createElement('div');
    knob.className = 'ftb-toggle-knob';
    toggle.append(knob);
    bindSkinSwitch(toggle, skin.id, skin.compatible);
    actions.append(toggle);
    const switcher = document.createElement('button');
    switcher.type = 'button';
    switcher.className = 'skin-preview-switch';
    switcher.dataset.skinPreviewSwitch = '';
    switcher.title = '切换明暗预览';
    switcher.textContent = '预览浅色';
    switcher.addEventListener('click', () => {
      previewOverrides[skin.id] = resolvedPreviewTheme(skin.id) === 'light' ? 'dark' : 'light';
      applyExternalCardTheme(skin);
    });
    actions.append(switcher);
    header.append(titles, actions);

    const previews = document.createElement('div');
    previews.className = 'skin-preview-list';
    const horizontal = document.createElement('div');
    horizontal.className = 'skin-preview-row skin-preview-horizontal';
    const horizontalStage = document.createElement('div');
    horizontalStage.className = 'skin-preview-stage';
    const horizontalHost = document.createElement('div');
    horizontalHost.className = `candidate wnd-h${previewClass ? ` ${previewClass}` : ''}`;
    horizontalHost.dataset.skinHorizontal = '';
    setPreviewPageArrows(horizontalHost, skin.id);
    if (previewHorizontalHtml) fillPreviewHost(horizontalHost, previewHorizontalHtml);
    horizontalStage.append(horizontalHost);
    horizontal.append(horizontalStage);
    const vertical = document.createElement('div');
    vertical.className = 'skin-preview-row skin-preview-vertical';
    const verticalStage = document.createElement('div');
    verticalStage.className = 'skin-preview-stage';
    const verticalHost = document.createElement('div');
    verticalHost.className = `candidate wnd-v${previewClass ? ` ${previewClass}` : ''}`;
    verticalHost.dataset.skinVertical = '';
    setPreviewPageArrows(verticalHost, skin.id);
    if (previewVerticalHtml) fillPreviewHost(verticalHost, previewVerticalHtml);
    verticalStage.append(verticalHost);
    vertical.append(verticalStage);
    const toolbar = document.createElement('div');
    toolbar.className = 'skin-preview-row skin-preview-toolbar';
    const toolbarStage = document.createElement('div');
    toolbarStage.className = 'skin-preview-stage';
    const toolbarHost = document.createElement('div');
    toolbarHost.className = `ftb-preview-host${previewClass ? ` ${previewClass}` : ''}`;
    toolbarHost.dataset.skinToolbar = '';
    fillToolbar(toolbarHost);
    toolbarStage.append(toolbarHost);
    toolbar.append(toolbarStage);
    previews.append(horizontal, vertical, toolbar);

    card.append(header, previews);
    injectGeneratedCandidateCss(skin);
    return card;
  }));
  empty.textContent = catalogScanned ? '没有发现外部皮肤。' : '尚未扫描。点击“刷新皮肤”读取皮肤目录。';
  empty.hidden = externalSkins.length !== 0;
  diagnostics.replaceChildren();
  if (scanIssues.length) {
    const details = document.createElement('details');
    const label = document.createElement('summary');
    label.textContent = `已忽略 ${scanIssues.length} 个无效皮肤目录`;
    const entries = document.createElement('ul');
    scanIssues.forEach((issue) => {
      const item = document.createElement('li');
      item.textContent = `${issue.folder}：${issue.reason}`;
      entries.append(item);
    });
    details.append(label, entries);
    diagnostics.append(details);
  }
  externalSkins.forEach(applyExternalCardTheme);
  syncSkinSwitches();
  syncAppearancePreviews();
}

export function applyCandidateSkinCatalog(
  skins: unknown, issues: unknown, directory: unknown, scanned: unknown, revision: unknown
): void {
  const key = JSON.stringify([skins, issues, directory, scanned, revision]);
  if (key === catalogKey) return;
  catalogKey = key;
  const nextRevision = typeof revision === 'number' && Number.isFinite(revision) ? revision : 0;
  if (nextRevision !== catalogRevision) resetExternalSkinStyles();
  externalSkins = Array.isArray(skins) ? skins.filter((skin): skin is ExternalSkin =>
    !!skin && typeof skin === 'object' && typeof skin.id === 'string' && typeof skin.name === 'string').map((skin) => ({
    ...skin,
    base: typeof skin.base === 'string' ? skin.base : 'fluent',
    layouts: Array.isArray(skin.layouts) ? skin.layouts : [],
    themes: Array.isArray(skin.themes) ? skin.themes : [],
    compatible: skin.compatible !== false
  })) : [];
  scanIssues = Array.isArray(issues) ? issues.filter((issue): issue is SkinScanIssue =>
    !!issue && typeof issue === 'object' && typeof issue.folder === 'string' && typeof issue.reason === 'string') : [];
  skinDirectory = typeof directory === 'string' ? directory : '';
  catalogScanned = scanned === true;
  catalogRevision = nextRevision;
  renderExternalSkins();
}

export function applyCandidateSkin(value: unknown): void { selectSkin(value, false); }

// Called with every config snapshot before applyCandidateSkin, which then refreshes the appearance previews.
export function applyBuiltinSkinPageArrows(value: unknown): void {
  const next: Record<string, boolean> = {};
  if (value && typeof value === 'object') {
    BUILTIN_SKINS.forEach((skin) => {
      next[skin] = (value as Record<string, unknown>)[skin] === true;
    });
  }
  builtinPageArrows = next;
  syncBuiltinCardPageArrows();
}

export function syncSkinPreviewTheme(theme: SkinPreviewTheme): void {
  activeTheme = theme;
  BUILTIN_SKINS.forEach((skin) => {
    previewOverrides[skin] = null;
    applyBuiltinCardTheme(skin, resolvedPreviewTheme(skin));
  });
  externalSkins.forEach((skin) => {
    previewOverrides[skin.id] = null;
    applyExternalCardTheme(skin);
  });
}

export async function setupSkin(): Promise<void> {
  previewHorizontalHtml = await loadHTML('/src/partials/candidate/candidate-wnd-h.html');
  previewVerticalHtml = await loadHTML('/src/partials/candidate/candidate-wnd-v.html');
  document.querySelectorAll<HTMLElement>('[data-skin-horizontal]').forEach((host) => {
    host.innerHTML = previewHorizontalHtml; limitCandidatePreview(host);
  });
  document.querySelectorAll<HTMLElement>('[data-skin-vertical]').forEach((host) => {
    host.innerHTML = previewVerticalHtml; limitCandidatePreview(host);
  });
  document.querySelectorAll<HTMLElement>('[data-skin-toolbar]').forEach(fillToolbar);
  skinInitialized = true;
  BUILTIN_SKINS.forEach((skin) => applyBuiltinCardTheme(skin, resolvedPreviewTheme(skin)));
  syncBuiltinCardPageArrows();
  syncSkinSwitches();
  syncAppearancePreviews();
  renderExternalSkins();

  document.querySelectorAll<HTMLElement>('[data-skin-switch]').forEach((element) => {
    if (!BUILTIN_SKINS.includes(element.dataset.skinSwitch as BuiltinSkin)) return;
    bindSkinSwitch(element, element.dataset.skinSwitch || 'fluent', true);
  });
  document.querySelectorAll<HTMLButtonElement>('[data-skin-preview-switch]').forEach((button) => {
    button.addEventListener('click', () => {
      const skin = asBuiltinSkin(button.closest<HTMLElement>('[data-candidate-skin]')?.dataset.candidateSkin);
      previewOverrides[skin] = resolvedPreviewTheme(skin) === 'light' ? 'dark' : 'light';
      applyBuiltinCardTheme(skin, previewOverrides[skin]!);
    });
  });
  document.getElementById('refreshExternalSkins')?.addEventListener('click', () => {
    window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'skinCatalogRequest' }));
  });
  document.getElementById('openExternalSkinDirectory')?.addEventListener('click', () => {
    window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'openSkinDirectory' }));
  });
}
