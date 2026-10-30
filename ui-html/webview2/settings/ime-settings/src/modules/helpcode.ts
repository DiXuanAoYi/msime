import { applyToggleState, setupDropdownMenu, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { updateCandidatePreviewHelpcode } from './appearance';
import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';
import { hoistOverlay } from '../utils/overlay-host';

const REFRESH_TIMEOUT_MS = 5000;
let toastTimer: number | null = null;
let pendingRefresh: (() => void) | null = null;

function showToast(message: string, ok: boolean, durationMs = 3200): void {
  const toast = document.getElementById('helpcodeToast');
  if (!toast) return;
  document.getElementById('helpcodeToastMessage')!.textContent = message;
  document.getElementById('helpcodeToastIcon')!.textContent = ok ? '' : '!';
  toast.className = `dict-toast visible ${ok ? 'success' : 'error'}`;
  if (toastTimer !== null) window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => { toast.classList.remove('visible'); toastTimer = null; }, durationMs);
}

// 等宿主推回重新扫描后的快照再提示；超时没回来就报失败。重复点击只保留最后一次。
function refreshCustomHelpcodes(): void {
  const webview = window.chrome?.webview;
  if (!webview) return;
  pendingRefresh?.();
  const timer = window.setTimeout(() => {
    finish();
    showToast('刷新失败，请稍后重试', false);
  }, REFRESH_TIMEOUT_MS);
  const unsubscribe = onHostMessage('configSnapshot', (message) => {
    finish();
    const schemas = (message.data as Record<string, any> | undefined)?.helpcode?.custom_schemas;
    const count = Array.isArray(schemas) ? schemas.length : 0;
    showToast(count > 0 ? `刷新成功，找到 ${count} 个自定义方案` : '刷新成功，文件夹里还没有自定义方案', true);
  });
  function finish(): void {
    window.clearTimeout(timer);
    unsubscribe();
    pendingRefresh = null;
  }
  pendingRefresh = finish;
  webview.postMessage(serializeHostMessage({ type: 'configRequest' }));
}

// 一组可多选的配置勾选框：最后一个勾着的取消不掉。
function setupAtLeastOneCheckboxes(checkboxes: [string, string][]): void {
  for (const [id, path] of checkboxes) {
    const checkbox = document.getElementById(id) as HTMLInputElement | null;
    checkbox?.addEventListener('change', () => {
      const anyChecked = checkboxes.some(
        ([otherId]) => (document.getElementById(otherId) as HTMLInputElement | null)?.checked);
      if (!anyChecked) {
        checkbox.checked = true;
        return;
      }
      updateConfig(path, checkbox.checked);
    });
  }
}

export function setupHelpcode(): void {
  // 双拼辅助码方案
  setupDropdownMenu('shuangpinHelpcodeSchemeBtn', 'shuangpinHelpcodeSchemeMenu', 'changeShuangpinScheme', true,
    'helpcode.shuangpin_helpcode_schema');

  // 全拼辅助码方案
  setupDropdownMenu('quanpinHelpcodeSchemeBtn', 'quanpinHelpcodeSchemeMenu', 'changeQuanpinScheme', true,
    'helpcode.quanpin_helpcode_schema');

  // 双拼辅助码开关
  setupToggleButton('shuangpinHelpcodeToggleBtn', (active) => {
    updateConfig('helpcode.shuangpin_helpcode', active);
    updateCandidatePreviewHelpcode({ shuangpin_helpcode: active });
  });

  // 双拼句中辅助码与直接辅助码（万象式）互斥：开一个就关另一个。宿主写配置时同样会关掉另一个并回推
  // 快照，这里先在页面上关掉，免得两个开关同时亮着一瞬。
  setupToggleButton('midSentenceHelpcodeToggleBtn', (active) => {
    if (active) applyToggleState('directHelpcodeToggleBtn', false);
    updateConfig('helpcode.shuangpin_mid_sentence_helpcode', active);
  });
  setupToggleButton('directHelpcodeToggleBtn', (active) => {
    if (active) applyToggleState('midSentenceHelpcodeToggleBtn', false);
    updateConfig('helpcode.shuangpin_direct_helpcode', active);
  });

  // 句中辅助码触发键，可多选；至少留一个，最后一个取消不掉
  setupAtLeastOneCheckboxes([
    ['midSentenceHelpcodeBacktickCheckbox', 'helpcode.shuangpin_mid_sentence_helpcode_backtick'],
    ['midSentenceHelpcodeSemicolonCheckbox', 'helpcode.shuangpin_mid_sentence_helpcode_semicolon'],
    ['midSentenceHelpcodeUppercaseCheckbox', 'helpcode.shuangpin_mid_sentence_helpcode_uppercase'],
  ]);

  // 直接辅助码句中四码的结束方式，同样可多选、至少留一个
  setupAtLeastOneCheckboxes([
    ['directHelpcodeSlashCheckbox', 'helpcode.shuangpin_direct_helpcode_slash'],
    ['directHelpcodeUppercaseCheckbox', 'helpcode.shuangpin_direct_helpcode_uppercase'],
  ]);

  // 全拼辅助码开关
  setupToggleButton('quanpinHelpcodeToggleBtn', (active) => {
    updateConfig('helpcode.quanpin_helpcode', active);
    updateCandidatePreviewHelpcode({ quanpin_helpcode: active });
  });

  // 是否在候选窗口中显示双拼辅助码
  setupToggleButton('showShuangpinHelpcodeToggleBtn', (active) => {
    updateConfig('helpcode.show_sp_helpcode_in_candidate_window', active);
    updateCandidatePreviewHelpcode({ show_sp_helpcode_in_candidate_window: active });
  });

  // 打开自定义辅助码所在文件夹，由宿主负责创建并用资源管理器打开
  document.getElementById('openCustomHelpcodeDirectory')?.addEventListener('click', () => {
    window.chrome?.webview?.postMessage(serializeHostMessage({ type: 'openHelpcodeDirectory' }));
  });
  // 重新请求配置快照：宿主会重新扫描 custom 文件夹，下拉框随快照重建
  document.getElementById('refreshCustomHelpcodes')?.addEventListener('click', refreshCustomHelpcodes);
  hoistOverlay(document.getElementById('helpcodeToast'));
  document.getElementById('helpcodeToastClose')?.addEventListener('click', () => {
    document.getElementById('helpcodeToast')?.classList.remove('visible');
  });

  // 是否在候选窗口中显示全拼辅助码
  setupToggleButton('showQuanpinHelpcodeToggleBtn', (active) => {
    updateConfig('helpcode.show_qp_helpcode_in_candidate_window', active);
    updateCandidatePreviewHelpcode({ show_qp_helpcode_in_candidate_window: active });
  });
}
