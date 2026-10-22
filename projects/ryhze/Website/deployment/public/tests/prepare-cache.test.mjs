import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdir, mkdtemp, readFile, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { preparePublicAssets } from '../prepare.mjs';

async function fixture(t, extraFiles = {}) {
  const root = await mkdtemp(join(tmpdir(), 'ryhze-public-cache-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const outputDirectory = join(root, 'deployment/public');
  await mkdir(outputDirectory, { recursive: true });
  await mkdir(join(root, 'public'), { recursive: true });
  const files = {
    'index.html': '<!doctype html><title>Public</title>',
    'entertainment/index.html': '<!doctype html><title>Entertainment</title>',
    '_astro/layout.AbCd1234.css': 'body{color:white}',
    '_astro/page.XyZ_5678.js': 'export const publicPage = true;',
    'fonts/Inter.woff2': 'synthetic font',
    'brand/logo.AbCd1234.js': 'public but not compiler generated',
    'build-info.json': '{"version":"development"}',
    ...extraFiles,
  };
  for (const [name, value] of Object.entries(files)) {
    await mkdir(dirname(join(root, 'dist', name)), { recursive: true });
    await writeFile(join(root, 'dist', name), value);
  }
  return { root, outputDirectory, files };
}
const json = async (fixture, name) =>
  JSON.parse(await readFile(join(fixture.outputDirectory, name), 'utf8'));

test('preparation records exact generated CSS/JS names and byte digests, never stable files', async (t) => {
  const f = await fixture(t, {
    '_astro/unhashed.js': 'unhashed',
    '_astro/image.AbCd1234.webp': 'image',
    '_astro/nested/script.AbCd1234.js': 'nested unknown convention',
  });
  const result = await preparePublicAssets(f);
  assert.equal(result.immutableAssets, 2);
  const manifest = await json(f, 'cache-manifest.json');
  assert.equal(manifest.schemaVersion, 1);
  assert.deepEqual(
    manifest.immutableAssets.map((entry) => entry.path),
    ['/_astro/layout.AbCd1234.css', '/_astro/page.XyZ_5678.js'],
  );
  for (const asset of manifest.immutableAssets) {
    const bytes = Buffer.from(f.files[asset.path.slice(1)]);
    assert.equal(asset.sha256, createHash('sha256').update(bytes).digest('hex'));
    assert.equal(asset.bytes, bytes.length);
  }
  const paths = await json(f, 'paths.json');
  assert.ok(paths.includes('/'));
  assert.ok(paths.includes('/entertainment'));
  assert.ok(paths.includes('/entertainment/'));
  assert.ok(paths.includes('/entertainment/index.html'));
  assert.ok(paths.includes('/fonts/Inter.woff2'));
  const before = await readFile(join(f.outputDirectory, 'cache-manifest.json'), 'utf8');
  await preparePublicAssets(f);
  assert.equal(await readFile(join(f.outputDirectory, 'cache-manifest.json'), 'utf8'), before);
});

test('a recorded fingerprinted URL cannot silently be reused for changed bytes', async (t) => {
  const f = await fixture(t);
  await preparePublicAssets(f);
  const before = await readFile(join(f.outputDirectory, 'cache-manifest.json'), 'utf8');
  await writeFile(join(f.root, 'dist/_astro/page.XyZ_5678.js'), 'changed script bytes');
  await assert.rejects(preparePublicAssets(f), /Fingerprinted URL reused for changed bytes/);
  assert.equal(await readFile(join(f.outputDirectory, 'cache-manifest.json'), 'utf8'), before);
  await rm(join(f.root, 'dist/_astro/page.XyZ_5678.js'));
  await writeFile(join(f.root, 'dist/_astro/page.New_1234.js'), 'changed script bytes');
  await preparePublicAssets(f);
  const manifest = await json(f, 'cache-manifest.json');
  assert.ok(manifest.immutableAssets.some((entry) => entry.path === '/_astro/page.New_1234.js'));
  assert.ok(!manifest.immutableAssets.some((entry) => entry.path === '/_astro/page.XyZ_5678.js'));
});

test('public-directory files cannot impersonate compiler fingerprints', async (t) => {
  const f = await fixture(t);
  await mkdir(join(f.root, 'public/_astro'));
  await writeFile(join(f.root, 'public/_astro/copied.AbCd1234.js'), 'not compiler generated');
  await assert.rejects(preparePublicAssets(f), /public\/_astro is reserved/);
});

test('private/server files are rejected before publishing either manifest', async (t) => {
  for (const name of [
    'api/index.html',
    'auth/session.json',
    '__auth/index.html',
    '__gate/index.html',
    '__agreements/index.html',
    'portal/index.html',
    'private/data.json',
    'account/index.html',
    'login/index.html',
    'hosting/config.json',
    '.env',
    '.git/config',
    'bundle.js.map',
    'schema.sql',
    'wrangler.toml',
  ]) {
    const f = await fixture(t, { [name]: 'private synthetic fixture' });
    await assert.rejects(preparePublicAssets(f), /Private\/server file/, name);
    await assert.rejects(readFile(join(f.outputDirectory, 'paths.json')), { code: 'ENOENT' });
    await assert.rejects(readFile(join(f.outputDirectory, 'cache-manifest.json')), {
      code: 'ENOENT',
    });
  }
});

test('ambiguous paths, asset-routing controls, symlinks and empty bundles fail closed', async (t) => {
  for (const name of [
    'space name.js',
    'encoded%2Fasset.js',
    '_headers',
    '_redirects',
    '_worker.js',
  ]) {
    const f = await fixture(t, { [name]: 'synthetic' });
    await assert.rejects(preparePublicAssets(f), /Unsupported public asset path/, name);
  }
  const empty = await fixture(t, { '_astro/empty.AbCd1234.js': '' });
  await assert.rejects(preparePublicAssets(empty), /Empty fingerprinted bundle/);
  const linked = await fixture(t);
  await symlink(join(linked.root, 'dist/index.html'), join(linked.root, 'dist/copied.html'));
  await assert.rejects(preparePublicAssets(linked), /must not contain symlinks/);
});
