import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyShortcutConfig, setupShortcut } from './shortcut';
import { updateConfig } from './config-sync';

vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));

class Checkbox extends EventTarget {
  checked = true;
}

let characterSet: Checkbox;
beforeEach(() => {
  vi.clearAllMocks();
  characterSet = new Checkbox();
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'characterSetShortcutCheckbox' ? characterSet : null,
    querySelectorAll: () => [],
  });
});
afterEach(() => vi.unstubAllGlobals());

it('applies the persisted shortcut setting without writing it back', () => {
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: false });
  expect(characterSet.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: true });
  expect(characterSet.checked).toBe(true);
});

it('writes only the character-set shortcut setting when the user disables it', () => {
  setupShortcut();
  characterSet.checked = false;
  characterSet.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_character_set_ctrl_shift_f', false);
});

it('accepts older configuration snapshots with no shortcut field', () => {
  applyShortcutConfig(undefined);
  applyShortcutConfig({ switch_language_shift: false });
  expect(characterSet.checked).toBe(true);
  expect(updateConfig).not.toHaveBeenCalled();
});
