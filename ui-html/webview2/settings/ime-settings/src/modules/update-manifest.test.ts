import { describe, expect, it } from 'vitest';
import { compareVersions, describeInstallerTrust, parseVersion, validateManifest,
  selectProductRelease, installedProductVersion, PRODUCT_RELEASES_URL } from './update-manifest';

const RELEASES = 'https://github.com/metasequoiaime/MSIME-Windows/releases';
const DIGEST = 'a'.repeat(64);

const manifest = (overrides: Record<string, unknown> = {}) => ({
  version: '0.3.1',
  releaseUrl: `${RELEASES}/tag/v0.3.1`,
  installerName: 'MetasequoiaIME_Setup_v0.3.1-unsigned.exe',
  installerSha256: DIGEST,
  signed: false,
  ...overrides
});

describe('parseVersion', () => {
  it('accepts the shapes the release pipeline produces', () => {
    expect(parseVersion('0.3.1')?.parts).toEqual([0, 3, 1]);
    expect(parseVersion('v0.3.1')?.display).toBe('0.3.1');
    expect(parseVersion(' 0.0.9.2 ')?.parts).toEqual([0, 0, 9, 2]);
    expect(parseVersion('1.2.3-beta.1')?.display).toBe('1.2.3-beta.1');
  });

  it('rejects anything it cannot order', () => {
    expect(parseVersion('')).toBeNull();
    expect(parseVersion('latest')).toBeNull();
    expect(parseVersion('v')).toBeNull();
  });
});

describe('compareVersions', () => {
  it('orders by numeric component, not lexically', () => {
    const order = (left: string, right: string) =>
      compareVersions(parseVersion(left)!, parseVersion(right)!);
    expect(order('0.10.0', '0.9.0')).toBeGreaterThan(0);
    expect(order('0.3.1', '0.3.1')).toBe(0);
    // A shorter version is not automatically older: 0.3 and 0.3.0 are the same release.
    expect(order('0.3', '0.3.0')).toBe(0);
    expect(order('0.3.1', '0.3')).toBeGreaterThan(0);
    expect(order('0.10.2-chat.10', '0.10.2-chat.2')).toBeGreaterThan(0);
    expect(order('0.10.2', '0.10.2-chat.2')).toBeGreaterThan(0);
  });
});

describe('personal release channel', () => {
  it('compares numeric installed file versions within the compiled chat channel', () => {
    const current = installedProductVersion('0.10.2')!;
    expect(current.display).toBe('0.10.2-chat.1');
    expect(compareVersions(parseVersion('0.10.2-chat.2')!, current)).toBeGreaterThan(0);
    expect(installedProductVersion('0.10.2-chat.3')?.display).toBe('0.10.2-chat.3');
  });
  const release = (tag: string, changes: Record<string, unknown> = {}) => ({
    draft: false, prerelease: true, tag_name: tag, html_url: `${PRODUCT_RELEASES_URL}/tag/${tag}`,
    assets: [{ state: 'uploaded', name: `MetasequoiaIME_Setup_${tag}.exe`, size: 123,
      digest: `sha256:${DIGEST}`,
      browser_download_url: `${PRODUCT_RELEASES_URL}/download/${tag}/MetasequoiaIME_Setup_${tag}.exe` }],
    ...changes
  });
  it('includes prereleases, orders chat revisions, and carries GitHub asset digests', () => {
    const selected = selectProductRelease([release('v0.10.2-chat.2'), release('v0.10.2-chat.10')]);
    expect(selected?.version.display).toBe('0.10.2-chat.10');
    expect(selected?.installerSha256).toBe(DIGEST);
  });
  it('rejects official channel, drafts, missing installers and external assets', () => {
    expect(selectProductRelease([release('v99.0.0')])).toBeNull();
    expect(selectProductRelease([release('v0.10.2-chat.1', { draft: true })])).toBeNull();
    expect(selectProductRelease([release('v0.10.2-chat.1', { assets: [] })])).toBeNull();
    expect(selectProductRelease([release('v0.10.2-chat.1', { html_url: 'https://github.com/metasequoiaime/msime-windows/releases/tag/v0.10.2' })])).toBeNull();
  });
});

describe('validateManifest', () => {
  it('accepts a well-formed manifest and carries the installer fields through', () => {
    const update = validateManifest(manifest(), RELEASES);
    expect(update?.version.display).toBe('0.3.1');
    expect(update?.installerSha256).toBe(DIGEST);
    expect(update?.signed).toBe(false);
  });

  it('rejects a release URL pointing anywhere else', () => {
    expect(validateManifest(manifest({ releaseUrl: 'https://attacker.example/x' }), RELEASES)).toBeNull();
    // A prefix match alone is not enough; a lookalike host must not pass.
    expect(validateManifest(manifest({ releaseUrl: `${RELEASES}.attacker.example/x` }), RELEASES)).toBeNull();
  });

  it('rejects a manifest with no usable version', () => {
    expect(validateManifest(manifest({ version: 'latest' }), RELEASES)).toBeNull();
    expect(validateManifest(manifest({ version: 42 }), RELEASES)).toBeNull();
  });

  it('ignores malformed optional fields rather than failing the whole check', () => {
    const update = validateManifest(
      manifest({ installerName: 'evil.exe; rm -rf /', installerSha256: 'nope', signed: 'yes' }),
      RELEASES
    );
    expect(update).not.toBeNull();
    expect(update?.installerName).toBeNull();
    expect(update?.installerSha256).toBeNull();
    expect(update?.signed).toBeNull();
  });

  it('still works for a manifest that predates the installer fields', () => {
    const update = validateManifest({ version: '0.3.1', releaseUrl: RELEASES }, RELEASES);
    expect(update?.installerSha256).toBeNull();
    expect(update?.signed).toBeNull();
  });
});

describe('describeInstallerTrust', () => {
  it('warns when the release is known to be unsigned', () => {
    const trust = describeInstallerTrust(validateManifest(manifest(), RELEASES)!);
    expect(trust.warning).toContain('未经代码签名');
    expect(trust.verify?.sha256).toBe(DIGEST);
    expect(trust.verify?.command).toContain('MetasequoiaIME_Setup_v0.3.1-unsigned.exe');
  });

  it('stays quiet once a release is signed, but still offers the digest', () => {
    const signed = manifest({ signed: true, installerName: 'MetasequoiaIME_Setup_v0.4.0.exe' });
    const trust = describeInstallerTrust(validateManifest(signed, RELEASES)!);
    expect(trust.warning).toBeNull();
    expect(trust.verify?.sha256).toBe(DIGEST);
  });

  it('says nothing about signing when the manifest does not report it', () => {
    const trust = describeInstallerTrust(validateManifest({ version: '1.0.0', releaseUrl: RELEASES }, RELEASES)!);
    expect(trust.warning).toBeNull();
    expect(trust.verify).toBeNull();
  });
});
