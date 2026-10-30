// Validation and trust reporting for this fork's GitHub releases, kept apart from the settings DOM
// so it can be tested. The manifest publishes four fields about the installer; the settings page used
// to read only two of them, so the digest the release pipeline had already computed never reached the
// person who was about to download an unsigned executable.

export type Version = {
  display: string;
  parts: number[];
  suffix?: string;
};

export type UpdateManifest = {
  version?: unknown;
  releaseUrl?: unknown;
  installerName?: unknown;
  installerSha256?: unknown;
  signed?: unknown;
};

export type ValidatedUpdate = {
  version: Version;
  releaseUrl: string;
  installerName: string | null;
  installerSha256: string | null;
  signed: boolean | null;
};

export function parseVersion(value: string): Version | null {
  const match = value.trim().match(/^v?(\d+(?:\.\d+)*)(?:-([\w.-]+))?(?:\+[\w.-]+)?$/i);
  if (!match) {
    return null;
  }

  const display = match[1];
  if (!display) {
    return null;
  }

  return {
    display: match[2] ? `${display}-${match[2]}` : display,
    parts: display.split('.').map(Number),
    suffix: match[2]
  };
}

export function compareVersions(left: Version, right: Version): number {
  const length = Math.max(left.parts.length, right.parts.length);
  for (let index = 0; index < length; index += 1) {
    const difference = (left.parts[index] ?? 0) - (right.parts[index] ?? 0);
    if (difference !== 0) {
      return difference;
    }
  }
  if (left.suffix === right.suffix) return 0;
  if (!left.suffix) return 1;
  if (!right.suffix) return -1;
  return left.suffix.localeCompare(right.suffix, 'en', { numeric: true });
}

export const PRODUCT_REPOSITORY = 'DiXuanAoYi/msime';
export const PRODUCT_CHANNEL = 'chat';
export const PRODUCT_RELEASE_REVISION = 1;
export const PRODUCT_RELEASES_URL = `https://github.com/${PRODUCT_REPOSITORY}/releases`;
export const PRODUCT_RELEASE_API = `https://api.github.com/repos/${PRODUCT_REPOSITORY}/releases?per_page=30`;

/** Windows file versions are numeric; preserve the compiled personal-channel revision when ordering releases. */
export function installedProductVersion(label: string): Version | null {
  const version = parseVersion(label);
  if (!version) return null;
  return version.suffix ? version : parseVersion(`${version.display}-${PRODUCT_CHANNEL}.${PRODUCT_RELEASE_REVISION}`);
}

/** Stable /latest excludes this branch's prereleases. Explicitly select the chat channel from the release list. */
export function selectProductRelease(value: unknown): ValidatedUpdate | null {
  if (!Array.isArray(value)) return null;
  let latest: ValidatedUpdate | null = null;
  for (const entry of value) {
    if (!entry || typeof entry !== 'object') continue;
    const release = entry as Record<string, unknown>;
    if (release.draft !== false || typeof release.tag_name !== 'string' ||
        !/^v\d+(?:\.\d+)*-chat\.\d+$/.test(release.tag_name) || !Array.isArray(release.assets)) continue;
    const tag = release.tag_name;
    const asset = release.assets.find((item: unknown) => {
      if (!item || typeof item !== 'object') return false;
      const row = item as Record<string, unknown>;
      return row.state === 'uploaded' && typeof row.name === 'string' && INSTALLER_NAME.test(row.name) &&
        row.name.includes(tag) && typeof row.size === 'number' && row.size > 0 &&
        row.browser_download_url === `${PRODUCT_RELEASES_URL}/download/${tag}/${row.name}`;
    }) as Record<string, unknown> | undefined;
    if (!asset || release.html_url !== `${PRODUCT_RELEASES_URL}/tag/${tag}`) continue;
    const digest = typeof asset.digest === 'string' && /^sha256:[0-9a-f]{64}$/.test(asset.digest)
      ? asset.digest.slice(7) : undefined;
    const update = validateManifest({ version: tag, releaseUrl: release.html_url,
      installerName: asset.name, installerSha256: digest,
      signed: typeof asset.name === 'string' && /-unsigned\.exe$/i.test(asset.name) ? false : undefined
    }, PRODUCT_RELEASES_URL);
    if (update && (!latest || compareVersions(update.version, latest.version) > 0)) latest = update;
  }
  return latest;
}

// The installer name reaches a command the user is told to paste, so it is restricted to the shape
// the release workflow produces rather than accepted as any string.
const INSTALLER_NAME = /^MetasequoiaIME_Setup_v[\w.-]+\.exe$/i;
const SHA256 = /^[0-9a-f]{64}$/;

export function validateManifest(manifest: UpdateManifest, releasesPageUrl: string): ValidatedUpdate | null {
  if (typeof manifest.version !== 'string' || typeof manifest.releaseUrl !== 'string') {
    return null;
  }
  if (manifest.releaseUrl !== releasesPageUrl && !manifest.releaseUrl.startsWith(`${releasesPageUrl}/`)) {
    return null;
  }
  const version = parseVersion(manifest.version);
  if (!version) {
    return null;
  }
  // The optional fields are reported when they are well formed and ignored otherwise. A manifest
  // that predates them, or that is served by something else, still yields a usable update check.
  return {
    version,
    releaseUrl: manifest.releaseUrl,
    installerName: typeof manifest.installerName === 'string' && INSTALLER_NAME.test(manifest.installerName)
      ? manifest.installerName
      : null,
    installerSha256: typeof manifest.installerSha256 === 'string' && SHA256.test(manifest.installerSha256)
      ? manifest.installerSha256
      : null,
    signed: typeof manifest.signed === 'boolean' ? manifest.signed : null
  };
}

export type InstallerTrust = {
  /** Shown prominently: the release is known not to carry a code signature. */
  warning: string | null;
  /** The command to run and the digest to expect, when the manifest published one. */
  verify: { command: string; sha256: string } | null;
};

export function describeInstallerTrust(update: ValidatedUpdate): InstallerTrust {
  const name = update.installerName ?? 'MetasequoiaIME_Setup_v<版本>.exe';
  return {
    warning: update.signed === false
      ? '该版本未经代码签名，SmartScreen 会拦截，且 uiAccess 失效（候选窗无法浮在以管理员身份运行的程序之上）。请务必核对下面的校验值。'
      : null,
    verify: update.installerSha256
      ? { command: `Get-FileHash .\\${name} -Algorithm SHA256`, sha256: update.installerSha256 }
      : null
  };
}
