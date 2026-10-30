import { serializeHostMessage } from '../../../../shared/messages';
import { updateConfig } from './config-sync';
import { setupToggleButton } from './shared';
import type { ValidatedUpdate } from './update-manifest';
import { compareVersions, describeInstallerTrust, installedProductVersion, selectProductRelease,
  PRODUCT_RELEASES_URL, PRODUCT_RELEASE_API } from './update-manifest';

const RELEASES_PAGE_URL = PRODUCT_RELEASES_URL;
const LICENSE_URL = 'https://github.com/DiXuanAoYi/msime/blob/feature/voice-reply-studio/LICENSE';
const PRIVACY_URL = 'https://github.com/DiXuanAoYi/msime/blob/feature/voice-reply-studio/PRIVACY.md';
const REQUEST_TIMEOUT_MS = 10000;

function postExternalUrl(url: string): void {
  if (window.chrome?.webview) {
    window.chrome.webview.postMessage(serializeHostMessage({ type: 'openExternalUrl', data: url }));
    return;
  }

  window.open(url, '_blank', 'noopener,noreferrer');
}

// The release pipeline publishes the installer's digest and whether it was signed. Showing both at
// the moment the user is deciding to download is the point: while the build is unsigned, that digest
// is the only integrity signal they have, and it was being thrown away.
function renderInstallerTrust(container: HTMLElement, update: ValidatedUpdate): void {
  const trust = describeInstallerTrust(update);
  container.replaceChildren();

  if (trust.warning) {
    const warning = document.createElement('p');
    warning.className = 'about-update-warning';
    warning.textContent = trust.warning;
    container.append(warning);
  }

  if (trust.verify) {
    const label = document.createElement('p');
    label.className = 'about-update-verify-label';
    label.textContent = '下载后请核对 SHA256：';

    const command = document.createElement('code');
    command.className = 'about-update-verify-command';
    command.textContent = trust.verify.command;

    const digest = document.createElement('code');
    digest.className = 'about-update-verify-digest';
    digest.textContent = trust.verify.sha256;

    container.append(label, command, digest);
  }
}

function setDialogOpen(dialog: HTMLDialogElement, open: boolean): void {
  if (open) {
    if (!dialog.open) {
      dialog.showModal();
    }
    return;
  }

  if (dialog.open) {
    dialog.close();
  }
}

export function setupAboutSettings(): void {
  setupToggleButton('serverDiagnosticLogToggleBtn', (active) => {
    updateConfig('general.diagnostic_log', active);
  });
  setupToggleButton('tsfDiagnosticLogToggleBtn', (active) => {
    updateConfig('general.tsf_diagnostic_log', active);
  });

  // These two rows shipped as buttons with no handler, so the settings page advertised a privacy
  // policy that did nothing when pressed. They are wired to the documents that actually govern the
  // software; the terms a user agrees to for an open-source product are its licence.
  for (const [id, url] of [
    ['about-license-link', LICENSE_URL],
    ['about-privacy-link', PRIVACY_URL],
    ['about-source-link', 'https://github.com/DiXuanAoYi/msime'],
    ['about-notices-link', 'https://github.com/DiXuanAoYi/msime/blob/feature/voice-reply-studio/THIRD_PARTY_NOTICES.txt'],
  ] as const) {
    const button = document.getElementById(id);
    if (button instanceof HTMLButtonElement) {
      button.addEventListener('click', () => postExternalUrl(url));
    } else {
      console.warn(`[about] document link ${id} not found`);
    }
  }

  const checkButton = document.getElementById('about-check-update');
  const versionLabel = document.querySelector('#about-settings .about-version');
  const statusLabel = document.getElementById('about-update-status');
  const dialog = document.getElementById('about-update-dialog');
  const dialogVersion = document.getElementById('about-update-dialog-version');
  const dialogTrust = document.getElementById('about-update-dialog-trust');
  const cancelButton = document.getElementById('about-update-cancel');
  const downloadButton = document.getElementById('about-update-download');

  if (!(checkButton instanceof HTMLButtonElement) ||
      !(versionLabel instanceof HTMLElement) ||
      !(statusLabel instanceof HTMLElement) ||
      !(dialog instanceof HTMLDialogElement) ||
      !(dialogVersion instanceof HTMLElement) ||
      !(dialogTrust instanceof HTMLElement) ||
      !(cancelButton instanceof HTMLButtonElement) ||
      !(downloadButton instanceof HTMLButtonElement)) {
    console.warn('[about] update controls not found');
    return;
  }

  let releaseUrl = RELEASES_PAGE_URL;

  const setStatus = (message: string, kind: 'idle' | 'success' | 'error' = 'idle') => {
    statusLabel.textContent = message;
    statusLabel.dataset.kind = kind;
  };

  cancelButton.addEventListener('click', () => setDialogOpen(dialog, false));
  downloadButton.addEventListener('click', () => {
    setDialogOpen(dialog, false);
    postExternalUrl(releaseUrl);
  });
  dialog.addEventListener('click', (event) => {
    if (event.target === dialog) {
      setDialogOpen(dialog, false);
    }
  });

  checkButton.addEventListener('click', async () => {
    const currentVersion = installedProductVersion(versionLabel.textContent ?? '');
    if (!currentVersion) {
      setStatus('无法识别当前版本', 'error');
      return;
    }

    checkButton.disabled = true;
    checkButton.textContent = '正在检查…';
    setStatus('');

    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);

    try {
      const response = await fetch(PRODUCT_RELEASE_API, {
        cache: 'no-store',
        headers: { Accept: 'application/vnd.github+json' },
        signal: controller.signal
      });
      if (!response.ok) {
        throw new Error(`Release query returned ${response.status}`);
      }

      const update = selectProductRelease(await response.json());
      if (!update) {
        throw new Error('No compatible personal-channel release');
      }

      const latest = update.version;
      if (compareVersions(latest, currentVersion) > 0) {
        releaseUrl = update.releaseUrl;
        dialogVersion.textContent = `v${latest.display}`;
        renderInstallerTrust(dialogTrust, update);
        setStatus(`发现新版本 v${latest.display}`, 'success');
        setDialogOpen(dialog, true);
      } else {
        setStatus('已是最新版本', 'success');
      }
    } catch (error) {
      console.warn('[about] update check failed:', error);
      setStatus('检查失败，可在你的仓库 Release 查看个人分支版本', 'error');
    } finally {
      window.clearTimeout(timeout);
      checkButton.disabled = false;
      checkButton.textContent = '检查更新';
    }
  });
}
