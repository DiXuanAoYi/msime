/// <reference types="node" />
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyCandidateFitContentMode,applyBuiltinSkinFitContent,skinFitsContent,applyBuiltinSkinPageArrows, applyCandidateSkin, applyCandidateSkinCatalog, skinShowsPageArrows } from './skin';

const readStyle = (path: string) => readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8');

it('keeps horizontal skin previews inside their card', () => {
  // The wrapper skin.ts inserts is not stretched by the column flexbox, so a max-content .container drags it past the card.
  expect(readStyle('../styles/modules/candidate/style-h.css'))
    .toMatch(/\.containerParent\s*\{\s*max-width:\s*100%;\s*min-width:\s*0;/);
  const skinCss = readStyle('../styles/modules/skin.css');
  for (const [, selector, body] of skinCss.matchAll(/([^{}]*\.wnd-h[^{}]*)\{([^}]*)\}/g)) {
    expect(`${selector.trim()} { ${body.trim()} }`).not.toMatch(/\bwidth:\s*max-content/);
  }
});

class PreviewElement {
  dataset: Record<string, string> = {};
  style = { setProperty: vi.fn(), removeProperty: vi.fn() };
  classList = { toggle: vi.fn(), contains: (name: string): boolean => name === 'caret-state-preview-host' };
  querySelector() { return null; }
}

let preview: PreviewElement;
let generatedCss = '';

beforeEach(() => {
  preview = new PreviewElement();
  generatedCss = '';
  const styles = new Map<string, { id: string }>();
  class Sheet {
    cssRules: Array<{ cssText?: string; cssRules?: unknown[]; insertRule?: (rule: string) => void }> = [];
    deleteRule(index: number) { this.cssRules.splice(index, 1); }
    insertRule(rule: string) {
      if (rule.startsWith('@scope')) {
        this.cssRules.push({ cssRules: [], insertRule: () => undefined });
      } else {
        this.cssRules.push({ cssText: rule });
      }
    }
    replaceSync(css: string) {
      generatedCss = css;
      this.cssRules = [{ cssText: css }];
    }
  }
  vi.stubGlobal('CSSStyleSheet', Sheet);
  vi.stubGlobal('document', {
    querySelectorAll: (selector: string) => selector === '.cand-preview .candidate' ? [preview] : [],
    getElementById: (id: string) => styles.get(id) ?? null,
    createElement: () => ({ id: '', dataset: {}, sheet: new Sheet() }),
    head: { appendChild: (style: { id: string }) => styles.set(style.id, style) }
  });
});

afterEach(() => vi.unstubAllGlobals());

it('applies built-in and custom candidate skins to the caret preview consumer', () => {
  applyCandidateSkin('wechat');
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-wechat', true);

  applyCandidateSkin('autumn_osmanthus');
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-autumn-osmanthus', true);
  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-wechat', false);

  applyCandidateSkinCatalog([
    {
      id: 'custom-blue', name: 'Custom Blue', version: '1', base: 'graphite', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: {
        dark: { surface: '#102030', border: '#405060', text: '#708090' },
        light: { surface: '#f0f1f2', border: '#d0d1d2', text: '#202122' }
      }
    }
  ], [], '', true, 0);
  applyCandidateSkin('custom-blue');

  expect(preview.classList.toggle).toHaveBeenCalledWith('skin-graphite', true);
  expect(preview.dataset.externalCaretSkinPreview).toBe('custom-blue');
  expect(preview.dataset.externalSkinPreview).toBeUndefined();
  expect(generatedCss).toContain(':is(:scope.candidate, :scope .candidate).theme-dark { --cand-bg: #102030; --cand-border: #405060; --cand-text: #708090; }');
  expect(generatedCss).toContain(':scope.caret-state-preview-host.theme-dark { --cand-bg: #102030; --cand-border: #405060; --cand-text: #708090; }');
  expect(generatedCss).toContain(':is(:scope.candidate, :scope .candidate).theme-light { --cand-bg: #f0f1f2; --cand-border: #d0d1d2; --cand-text: #202122; }');
  expect(generatedCss).toContain(':scope.caret-state-preview-host.theme-light { --cand-bg: #f0f1f2; --cand-border: #d0d1d2; --cand-text: #202122; }');
});

it('previews number and translation colours from the skin manifest', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-gloss', name: 'Custom Gloss', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: {
        dark: { number: '#8899aa', translation: '#e6a817' },
        light: { translation: 'red; } body { display: none' }
      }
    }
  ], [], '', true, 1);
  applyCandidateSkin('custom-gloss');

  expect(generatedCss).toContain(':is(:scope, :scope .candidate) .num, :is(:scope, :scope .candidate) .cand-no { color: #8899aa; }');
  expect(generatedCss).toContain(':is(:scope, :scope .candidate) .cand-translation { color: #e6a817; opacity: 1; }');
  expect(generatedCss).not.toContain('display: none');
});

it('anchors preview colour rules on the host so the light theme and selected bar apply', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-pink', name: 'Custom Pink', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: {
        dark: { accent: '#e08aa8', selected: '#442233' },
        light: { accent: '#c45c7a', selected: '#f5dde5' }
      }
    }
  ], [], '', true, 2);
  applyCandidateSkin('custom-pink');

  expect(generatedCss).toContain(':is(:scope, :scope .candidate) .cursor, :is(:scope, :scope .candidate) .first::before { background: #e08aa8; }');
  expect(generatedCss).toContain(':is(:scope, :scope .candidate).theme-light .cursor, :is(:scope, :scope .candidate).theme-light .first::before { background: #c45c7a; }');
  // Hovering the selected candidate keeps the selected colour instead of the base skin's hover colour.
  expect(generatedCss).toContain(
    ':is(:scope, :scope .candidate).theme-light .first, :is(:scope, :scope .candidate).theme-light .cand.first, :is(:scope, :scope .candidate).theme-light .cand.first:hover { background-color: #f5dde5; }');
});

it('previews decoration placement, background image and corner radius from the skin manifest', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-art', name: 'Custom Art', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      decorationTopDip: 88, decorationWidthDip: 136, decorationImage: 'assets/character.png', decorationAlign: 'left',
      backgroundImage: 'assets/paper.png', backgroundFit: 'contain', backgroundOpacity: 0.5, cornerRadiusDip: 12
    }
  ], [], '', true, 3);
  applyCandidateSkin('custom-art');

  expect(generatedCss).toContain('min-width: max(7em, var(--msime-skin-min-width, 0px), var(--msime-skin-decoration-width, 0px))');
  expect(generatedCss).toContain('top: 0; left: 0;');
  expect(generatedCss).toContain('height: var(--msime-skin-decoration-top, 0px)');
  expect(generatedCss).toContain('/custom-art/assets/character.png');
  // The image sits in the card's own border-box background so the border is drawn over it, as in the D2D renderer;
  // the opacity becomes a veil of the surface over it.
  const veil = 'color-mix(in srgb, var(--cand-bg) 50%, transparent)';
  expect(generatedCss).toContain(
    `:is(:scope, :scope .candidate) .container:not(:empty) { background-image: linear-gradient(${veil}, ${veil}), url("https://candidate-skins.example/custom-art/assets/paper.png?v=3"); background-size: auto, contain;`);
  expect(generatedCss).toContain('background-origin: border-box; background-clip: border-box; }');
  expect(generatedCss).not.toContain('::before {\n  content: ""; position: absolute; inset: 0');
  expect(generatedCss).toContain(':is(:scope, :scope .candidate) .container { border-radius: 12px; --wg-radius: 12px; --ao-radius: 12px; }');
  expect(generatedCss).toContain(':is(:scope, :scope .candidate).wnd-h .container > .pinyin + .row-wrapper > .cand { border-bottom-left-radius: 12px; }');
});

it('veils the background image with the surface the card is painted with in each theme', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-veil', name: 'Custom Veil', version: '1', base: 'willow_green', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true, backgroundImage: 'paper.png', backgroundOpacity: 0.9,
      candidate: { light: { surface: '#fff7fa' } }
    }
  ], [], '', true, 8);
  applyCandidateSkin('custom-veil');

  expect(generatedCss).toContain(
    ':is(:scope, :scope .candidate) .container:not(:empty) { background-image: linear-gradient(color-mix(in srgb, var(--wg-surface) 10%, transparent)');
  expect(generatedCss).toContain(
    ':is(:scope, :scope .candidate).theme-light .container:not(:empty) { background-image: linear-gradient(color-mix(in srgb, #fff7fa 10%, transparent)');
  // The image rules come after the surface rules, whose `background` shorthand would otherwise reset the layers.
  expect(generatedCss.indexOf('.container:not(:empty) { background-image'))
    .toBeGreaterThan(generatedCss.indexOf(':is(:scope, :scope .candidate).theme-light .container { background: #fff7fa; }'));
});

it('keys theme colours on a candidate host nested inside the skin card', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-card', name: 'Custom Card', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: { light: { border: 'rgba(176, 80, 110, 0.22)' } }
    }
  ], [], '', true, 9);
  applyCandidateSkin('custom-card');

  // In the skin list the scope root is the card and the theme class sits on the .candidate inside it.
  expect(generatedCss).toContain(
    ':is(:scope, :scope .candidate).theme-light .container { border-color: rgba(176, 80, 110, 0.22); }');
  expect(generatedCss).toContain(
    ':is(:scope.candidate, :scope .candidate).theme-light { --cand-border: rgba(176, 80, 110, 0.22); }');
});

it('ignores out-of-range numbers instead of pasting them into preview css', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-bad', name: 'Custom Bad', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark'], compatible: true, backgroundImage: 'bg.png',
      backgroundOpacity: '1; } body { display: none', cornerRadiusDip: '4px; } body { display: none'
    }
  ], [], '', true, 4);
  applyCandidateSkin('custom-bad');

  // A bad opacity falls back to 1, which leaves no veil over the image.
  expect(generatedCss).toContain('color-mix(in srgb, var(--cand-bg) 0%, transparent)');
  expect(generatedCss).not.toContain('display: none');
  expect(generatedCss).not.toContain('border-radius: 4px');
});
it('draws no decoration without a decoration image', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-preview-only', name: 'Preview Only', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark'], compatible: true, decorationTopDip: 88, decorationWidthDip: 136
    }
  ], [], '', true, 5);
  applyCandidateSkin('custom-preview-only');

  expect(generatedCss).not.toContain('.containerParent:not(:empty)::before');
});

it('previews toolbar colours and corner radius per theme from the skin manifest', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-toolbar', name: 'Custom Toolbar', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true, toolbarCornerRadiusDip: 12,
      toolbar: {
        dark: { border: 'rgba(224, 138, 168, 0.38)', handle: '#e08aa8' },
        light: { background: '#fff7fa', icon: '#3a2a30', divider: 'red; } body { display: none' }
      }
    }
  ], [], '', true, 6);
  applyCandidateSkin('custom-toolbar');

  const dark = ':is(:scope.ftb-preview-host, :scope .ftb-preview-host).theme-dark';
  const light = ':is(:scope.ftb-preview-host, :scope .ftb-preview-host).theme-light';
  expect(generatedCss).toContain(`${dark} .status-bar { border-color: rgba(224, 138, 168, 0.38); }`);
  expect(generatedCss).toContain(`${dark} .drag-handle { background: #e08aa8; }`);
  expect(generatedCss).toContain(`${dark} .status-bar { border-radius: calc(12px * var(--ftb-scale)); }`);
  expect(generatedCss).toContain(`${light} .status-bar { background-color: #fff7fa; }`);
  expect(generatedCss).toContain(`${light} .icon { color: #3a2a30; }`);
  expect(generatedCss).not.toContain(`${light} .drag-handle`);
  expect(generatedCss).not.toContain('display: none');
});

it('previews detailed candidate colours with the user text colour still winning', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-detail', name: 'Custom Detail', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      candidate: {
        dark: {
          candidateText: '#dddddd', preeditText: '#aaaaaa', preeditCaret: '#ff8800', selectedBar: '#00aaff',
          selectedText: '#ffffff', selectedNumber: '#cccccc', selectedTranslation: '#ffe0a0'
        },
        light: { preeditCaret: 'red; } body { display: none' }
      }
    }
  ], [], '', true, 10);
  applyCandidateSkin('custom-detail');

  const host = ':is(:scope, :scope .candidate)';
  expect(generatedCss).toContain(`${host} :where(.cand) .text { color: var(--msime-user-text, #dddddd); }`);
  expect(generatedCss).toContain(`${host} .pinyin .text { color: var(--msime-user-text, #aaaaaa); }`);
  expect(generatedCss).toContain(`${host} .cursor { background: #ff8800; }`);
  expect(generatedCss).toContain(`${host} .first::before { background: #00aaff; }`);
  expect(generatedCss).toContain(`${host} .cand.first .text { color: #ffffff; }`);
  expect(generatedCss).toContain(`${host} .cand.first .num, ${host} .cand.first .cand-no { color: #cccccc; }`);
  expect(generatedCss).toContain(`${host} .cand.first .cand-translation { color: #ffe0a0; opacity: 1; }`);
  expect(generatedCss).not.toContain('display: none');
});

it('previews preedit band, frame width, highlight corners, shadow and font from the skin manifest', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-shape', name: 'Custom Shape', version: '1', base: 'willow_green', layouts: ['horizontal'],
      themes: ['dark', 'light'], compatible: true,
      borderWidthDip: 2, itemCornerRadiusDip: 8, shadow: 'soft', fontFamily: 'LXGW WenKai',
      candidate: { dark: { preeditBackground: 'rgba(255, 255, 255, 0.08)', preeditDivider: '#444444' } }
    }
  ], [], '', true, 11);
  applyCandidateSkin('custom-shape');

  const host = ':is(:scope, :scope .candidate)';
  expect(generatedCss).toContain(`${host} .row.pinyin { background: rgba(255, 255, 255, 0.08); border-radius: 8px; }`);
  expect(generatedCss).toContain(`${host} .row.pinyin { border-bottom: 1px solid #444444; }`);
  // Willow green has no frame of its own, so the width comes with a style and its transparent palette colour.
  expect(generatedCss).toContain(`${host} .container:not(:empty) { border-width: 2px; border-style: solid; border-color: transparent; }`);
  expect(generatedCss).toContain(`${host} .container .cand, ${host} .container .cand.first { border-radius: 8px; }`);
  // Willow green clips .container, so the shadow goes on its parent.
  expect(generatedCss).toContain(`${host} .containerParent:not(:empty) { box-shadow: 8px 10px 24px rgba(0, 0, 0, 0.170), 2px 3px 8px rgba(0, 0, 0, 0.110); }`);
  expect(generatedCss).toContain(`${host}.theme-light .containerParent:not(:empty) { box-shadow: 8px 10px 24px rgba(0, 0, 0, 0.090), 2px 3px 8px rgba(0, 0, 0, 0.050); }`);
  expect(generatedCss).toContain('font-family: "LXGW WenKai", var(--cand-font-family, inherit) !important;');
});

it('drops out-of-range geometry and unsafe font names from preview css', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-unsafe', name: 'Custom Unsafe', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark'], compatible: true, borderWidthDip: 5, itemCornerRadiusDip: 17, shadow: 'huge',
      fontFamily: 'Foo"; } body { display: none'
    }
  ], [], '', true, 12);
  applyCandidateSkin('custom-unsafe');

  expect(generatedCss).not.toContain('border-width');
  expect(generatedCss).not.toContain('box-shadow');
  expect(generatedCss).not.toContain('font-family');
  expect(generatedCss).not.toContain('display: none');
});

it('emits no toolbar rules for a skin without a toolbar table', () => {
  applyCandidateSkinCatalog([
    {
      id: 'custom-no-toolbar', name: 'No Toolbar', version: '1', base: 'fluent', layouts: ['horizontal'],
      themes: ['dark'], compatible: true, toolbarCornerRadiusDip: null
    }
  ], [], '', true, 7);
  applyCandidateSkin('custom-no-toolbar');

  expect(generatedCss).not.toContain('ftb-preview-host');
});

it('shows page arrows in the appearance preview only for skins that turn them on', () => {
  // A candidate preview, not the caret-state one, is the host that carries the arrows.
  preview.classList.contains = () => false;
  applyBuiltinSkinPageArrows({ fluent: false, graphite: true, wechat: 'yes' });
  expect(skinShowsPageArrows('graphite')).toBe(true);
  expect(skinShowsPageArrows('wechat')).toBe(false);

  applyCandidateSkin('graphite');
  expect(preview.classList.toggle).toHaveBeenLastCalledWith('page-arrows-on', true);
  applyCandidateSkin('fluent');
  expect(preview.classList.toggle).toHaveBeenLastCalledWith('page-arrows-on', false);

  // An external skin carries the value the host already resolved against its base.
  applyCandidateSkinCatalog([
    { id: 'custom-arrows', name: 'Arrows', version: '1', base: 'fluent', layouts: ['vertical'], themes: ['dark'],
      compatible: true, pageArrows: true }
  ], [], '', true, 8);
  applyCandidateSkin('custom-arrows');
  expect(preview.classList.toggle).toHaveBeenLastCalledWith('page-arrows-on', true);

  // Without the host's map every built-in skin keeps the arrows off.
  applyBuiltinSkinPageArrows(undefined);
  expect(skinShowsPageArrows('graphite')).toBe(false);
});

it('follows skin content width unless explicitly overridden',()=>{applyBuiltinSkinFitContent({fluent:true,wechat:false});applyCandidateFitContentMode('follow');expect(skinFitsContent('fluent')).toBe(true);expect(skinFitsContent('wechat')).toBe(false);applyCandidateFitContentMode('disabled');expect(skinFitsContent('fluent')).toBe(false);applyCandidateFitContentMode('enabled');expect(skinFitsContent('wechat')).toBe(true);applyCandidateFitContentMode('follow');});

it('uses separate word class and separator colors for normal and selected candidate translations',()=>{applyCandidateSkinCatalog([{id:'custom-parts',name:'Parts',version:'1',base:'fluent',layouts:['horizontal'],themes:['dark'],compatible:true,candidate:{dark:{translationPos:'#123456',translationSeparator:'#234567',selectedTranslationPos:'#345678',selectedTranslationSeparator:'#456789'}}}],[],'',true,9);applyCandidateSkin('custom-parts');expect(generatedCss).toContain('.cand-translation .cand-pos {color:#123456;opacity:1;}');expect(generatedCss).toContain('.cand-translation .cand-sep {color:#234567;opacity:1;}');expect(generatedCss).toContain('.cand.first .cand-translation .cand-pos {color:#345678;opacity:1;}');expect(generatedCss).toContain('.cand.first .cand-translation .cand-sep {color:#456789;opacity:1;}');});
