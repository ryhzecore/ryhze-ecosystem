import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import worker from '../worker.mjs';
import pkg from '../../../package.json' with { type: 'json' };
import manifest from '../cache-manifest.json' with { type: 'json' };

const IMMUTABLE = 'public, max-age=31536000, immutable';
const REVALIDATE = 'public, max-age=0, must-revalidate';
const firstBundle = () => {
  assert.ok(manifest.immutableAssets.length > 0, 'Build and prepare assets before testing');
  return manifest.immutableAssets[0];
};
const typeFor = (path) => (path.endsWith('.css') ? 'text/css' : 'text/javascript');
function bindings({ status = 200, headers = {}, body = 'public asset' } = {}) {
  const calls = [];
  const legacyResponse = new Response('private legacy', {
    headers: { 'Cache-Control': 'private, no-store', 'Set-Cookie': 'session=synthetic; HttpOnly' },
  });
  return {
    calls,
    legacyResponse,
    ASSETS: {
      async fetch(request) {
        calls.push(['assets', request]);
        return new Response([204, 304].includes(status) ? null : body, { status, headers });
      },
    },
    LEGACY: {
      async fetch(request) {
        calls.push(['legacy', request]);
        return legacyResponse;
      },
    },
  };
}

async function withManifest(paths, immutableAssets, schemaVersion = 1) {
  // Exercise corrupted/stale manifest inputs without mutating checked-in files.
  const source = (await readFile(new URL('../worker.mjs', import.meta.url), 'utf8'))
    .replace(/import pkg from [^;]+;/, `const pkg = ${JSON.stringify({ version: pkg.version })};`)
    .replace(/import publicPaths from [^;]+;/, `const publicPaths = ${JSON.stringify(paths)};`)
    .replace(
      /import cacheManifest from [^;]+;/,
      `const cacheManifest = ${JSON.stringify({ schemaVersion, immutableAssets })};`,
    );
  return (await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`))
    .default;
}

test('generated immutable inventory exactly matches current build bytes and public paths', async () => {
  const paths = JSON.parse(await readFile(new URL('../paths.json', import.meta.url), 'utf8'));
  firstBundle();
  for (const asset of manifest.immutableAssets) {
    assert.match(asset.path, /^\/_astro\/[^/]+\.[A-Za-z0-9_-]{8,64}\.(?:css|js|mjs)$/);
    assert.ok(paths.includes(asset.path));
    const bytes = await readFile(new URL(`../../../dist${asset.path}`, import.meta.url));
    assert.equal(asset.sha256, createHash('sha256').update(bytes).digest('hex'));
    assert.equal(asset.bytes, bytes.length);
  }
});

test('cold verified bundles get immutable caching and retain body, validators and security headers', async () => {
  for (const asset of manifest.immutableAssets) {
    for (const method of ['GET', 'HEAD']) {
      const bytes = await readFile(new URL(`../../../dist${asset.path}`, import.meta.url));
      const e = bindings({
        body: bytes,
        headers: {
          'Content-Type': typeFor(asset.path),
          'Cache-Control': REVALIDATE,
          ETag: `"${asset.sha256}"`,
          'Last-Modified': 'Thu, 01 Oct 2026 00:00:00 GMT',
          Vary: 'Accept-Encoding',
        },
      });
      const req = new Request(`https://ryhze.com${asset.path}?public-build=1`, { method });
      const result = await worker.fetch(req, e);
      assert.equal(e.calls[0][1], req);
      assert.equal(result.status, 200);
      assert.equal(result.headers.get('Cache-Control'), IMMUTABLE);
      assert.equal(result.headers.get('ETag'), `"${asset.sha256}"`);
      assert.equal(result.headers.get('Last-Modified'), 'Thu, 01 Oct 2026 00:00:00 GMT');
      assert.equal(result.headers.get('Vary'), 'Accept-Encoding');
      assert.equal(result.headers.get('X-Content-Type-Options'), 'nosniff');
      assert.equal(result.headers.get('X-Frame-Options'), 'DENY');
      assert.equal(result.headers.get('X-Robots-Tag'), 'noindex, nofollow, noarchive');
      assert.equal(result.headers.get('Referrer-Policy'), 'strict-origin-when-cross-origin');
      assert.equal(
        result.headers.get('Permissions-Policy'),
        'camera=(), microphone=(), geolocation=()',
      );
      assert.deepEqual(
        new Uint8Array(await result.arrayBuffer()),
        method === 'HEAD' ? new Uint8Array() : new Uint8Array(bytes),
      );
    }
  }
});

test('HTML, metadata and stable font/image names require revalidation', async () => {
  for (const path of [
    '/',
    '/entertainment/',
    '/index.html',
    '/build-info.json',
    '/version.json',
    '/robots.txt',
    '/fonts/Inter.woff2',
    '/brand/web/symbol-480.webp',
  ]) {
    const e = bindings({ headers: { ETag: '"mutable-v1"', 'Content-Type': 'text/plain' } });
    const result = await worker.fetch(new Request(`https://www.ryhze.com${path}`), e);
    assert.equal(e.calls[0][0], 'assets', path);
    assert.equal(result.headers.get('Cache-Control'), REVALIDATE, path);
    assert.equal(result.headers.get('ETag'), '"mutable-v1"');
    assert.equal(await result.text(), 'public asset');
  }
});

test('conditional GET/HEAD passes both validators unchanged and keeps bodyless 304 semantics', async () => {
  for (const [path, policy] of [
    [firstBundle().path, IMMUTABLE],
    ['/fonts/Inter.woff2', REVALIDATE],
    ['/', REVALIDATE],
  ]) {
    for (const method of ['GET', 'HEAD']) {
      const e = bindings({
        status: 304,
        headers: { ETag: '"existing"', 'Last-Modified': 'Thu, 01 Oct 2026 00:00:00 GMT' },
      });
      const request = new Request(`https://ryhze.com${path}`, {
        method,
        headers: {
          'If-None-Match': '"existing"',
          'If-Modified-Since': 'Thu, 01 Oct 2026 00:00:00 GMT',
        },
      });
      const result = await worker.fetch(request, e);
      assert.equal(e.calls[0][1], request);
      assert.equal(e.calls[0][1].headers.get('If-None-Match'), '"existing"');
      assert.equal(e.calls[0][1].headers.get('If-Modified-Since'), 'Thu, 01 Oct 2026 00:00:00 GMT');
      assert.equal(result.status, 304);
      assert.equal(await result.text(), '');
      assert.equal(result.headers.get('ETag'), '"existing"');
      assert.equal(result.headers.get('Cache-Control'), policy);
    }
  }
});

test('a mutable URL can deliver changed bytes and a new ETag after revalidation', async () => {
  let current = { body: 'font-v1', etag: '"v1"' };
  const env = {
    ASSETS: {
      async fetch(request) {
        const unchanged = request.headers.get('If-None-Match') === current.etag;
        return new Response(unchanged ? null : current.body, {
          status: unchanged ? 304 : 200,
          headers: { ETag: current.etag },
        });
      },
    },
  };
  let result = await worker.fetch(new Request('https://ryhze.com/fonts/Inter.woff2'), env);
  assert.equal(await result.text(), 'font-v1');
  assert.equal(result.headers.get('Cache-Control'), REVALIDATE);
  const request = () =>
    new Request('https://ryhze.com/fonts/Inter.woff2', { headers: { 'If-None-Match': '"v1"' } });
  result = await worker.fetch(request(), env);
  assert.equal(result.status, 304);
  current = { body: 'font-v2', etag: '"v2"' };
  result = await worker.fetch(request(), env);
  assert.equal(result.status, 200);
  assert.equal(result.headers.get('ETag'), '"v2"');
  assert.equal(await result.text(), 'font-v2');
  assert.equal(result.headers.get('Cache-Control'), REVALIDATE);
});

test('credentialed public requests never receive storage permission', async () => {
  const path = firstBundle().path;
  for (const name of [
    'Authorization',
    'Cookie',
    'CF-Access-Jwt-Assertion',
    'CF-Access-Authenticated-User-Email',
  ]) {
    for (const target of ['/', path]) {
      const e = bindings({ headers: { 'Content-Type': typeFor(path) } });
      const req = new Request(`https://ryhze.com${target}`, {
        headers: { [name]: 'synthetic-value' },
      });
      const result = await worker.fetch(req, e);
      assert.equal(e.calls[0][1], req);
      assert.equal(result.headers.get('Cache-Control'), 'private, no-store', name);
    }
  }
});

test('personalized response headers and restrictive cache directives fail closed', async () => {
  const path = firstBundle().path;
  for (const headers of [
    { 'Set-Cookie': 'session=synthetic; Secure; HttpOnly' },
    { 'WWW-Authenticate': 'Bearer' },
    { 'Cache-Control': 'private, max-age=3600' },
    { 'Cache-Control': 'public, no-store' },
    { 'Cache-Control': 'private="Set-Cookie"' },
    { 'CDN-Cache-Control': 'no-store' },
    { 'Cloudflare-CDN-Cache-Control': 'private' },
    { 'Surrogate-Control': 'no-store' },
    { Vary: 'Cookie' },
    { Vary: 'Accept-Encoding, Authorization' },
    { Vary: '*' },
    { Vary: 'X-User-Id' },
  ]) {
    const e = bindings({ headers: { 'Content-Type': typeFor(path), ...headers } });
    const result = await worker.fetch(new Request(`https://ryhze.com${path}`), e);
    assert.equal(result.headers.get('Cache-Control'), 'private, no-store', JSON.stringify(headers));
    if (headers['Set-Cookie'])
      assert.equal(result.headers.get('Set-Cookie'), headers['Set-Cookie']);
  }
});

test('redirects, failures, partial content and wrong bundle MIME cannot become immutable', async () => {
  const path = firstBundle().path;
  for (const status of [204, 206, 301, 302, 307, 308, 400, 401, 403, 404, 416, 500, 503]) {
    const e = bindings({ status, headers: { 'Content-Type': typeFor(path), Location: '/login/' } });
    const result = await worker.fetch(new Request(`https://ryhze.com${path}`), e);
    assert.equal(result.status, status);
    assert.equal(result.headers.get('Cache-Control'), 'no-store', String(status));
  }
  for (const type of ['text/html', 'application/json', 'text/plain']) {
    const e = bindings({ headers: { 'Content-Type': type } });
    const result = await worker.fetch(new Request(`https://ryhze.com${path}`), e);
    assert.equal(result.headers.get('Cache-Control'), 'no-store', type);
  }
  const e = bindings({
    headers: { 'Content-Type': typeFor(path), 'Content-Range': 'bytes 0-9/20' },
  });
  const result = await worker.fetch(
    new Request(`https://ryhze.com${path}`, { headers: { Range: 'bytes=0-9' } }),
    e,
  );
  assert.equal(result.headers.get('Cache-Control'), 'no-store');
});

test('upstream no-cache is respected and conflicting CDN controls are not forwarded', async () => {
  const path = firstBundle().path;
  const e = bindings({
    headers: {
      'Content-Type': typeFor(path),
      'Cache-Control': 'no-cache',
      'CDN-Cache-Control': 'public, max-age=31536000',
      'Cloudflare-CDN-Cache-Control': 'public, max-age=31536000',
      'Surrogate-Control': 'max-age=31536000',
    },
  });
  const result = await worker.fetch(new Request(`https://ryhze.com${path}`), e);
  assert.equal(result.headers.get('Cache-Control'), REVALIDATE);
  for (const name of ['CDN-Cache-Control', 'Cloudflare-CDN-Cache-Control', 'Surrogate-Control'])
    assert.equal(result.headers.get(name), null);
});

test('immutable caching requires a valid exact manifest entry and strict Astro bundle shape', async () => {
  const valid = { path: '/_astro/example.AbCd1234.js', sha256: 'a'.repeat(64), bytes: 100 };
  for (const [entry, schema] of [
    [{ ...valid, sha256: '' }, 1],
    [{ ...valid, bytes: 0 }, 1],
    [{ ...valid, bytes: -1 }, 1],
    [{ ...valid, path: '/brand/example.AbCd1234.js' }, 1],
    [{ ...valid, path: '/_astro/example.js' }, 1],
    [{ ...valid, path: '/_astro/nested/example.AbCd1234.js' }, 1],
    [{ ...valid, path: '/_astro/example.AbCd1234.html' }, 1],
    [valid, 2],
  ]) {
    const instance = await withManifest([entry.path], [entry], schema);
    const e = bindings({ headers: { 'Content-Type': 'text/javascript' } });
    const result = await instance.fetch(new Request(`https://ryhze.com${entry.path}`), e);
    assert.equal(result.headers.get('Cache-Control'), REVALIDATE, JSON.stringify(entry));
  }
  const instance = await withManifest([valid.path], []);
  const result = await instance.fetch(new Request(`https://ryhze.com${valid.path}`), bindings());
  assert.equal(result.headers.get('Cache-Control'), REVALIDATE);
});

test('private/API paths always preserve legacy responses even if accidentally added to the allowlist', async () => {
  for (const path of [
    '/api',
    '/api/auth/',
    '/auth/login/',
    '/__auth/',
    '/__gate/',
    '/portal/',
    '/account/',
    '/private/file.js',
    '/login/',
    '/session.json',
  ]) {
    const instance = await withManifest([path], []);
    const e = bindings();
    const request = new Request(`https://ryhze.com${path}`, {
      headers: { Cookie: 'synthetic=session', Authorization: 'Bearer synthetic' },
    });
    const result = await instance.fetch(request, e);
    assert.equal(e.calls[0][0], 'legacy', path);
    assert.equal(e.calls[0][1], request);
    assert.equal(result, e.legacyResponse);
  }
});

test('unlisted hash-looking assets, encoded spellings and private endpoints retain legacy behavior', async () => {
  for (const path of [
    '/_astro/unknown.AbCd1234.js',
    '/_astro/nested/unknown.AbCd1234.js',
    '/api/catalog.json',
    '/auth/session',
    '/%61pi/catalog.json',
    '/brand/not-built.12345678.png',
  ]) {
    const e = bindings();
    const request = new Request(`https://ryhze.com${path}`);
    const result = await worker.fetch(request, e);
    assert.equal(e.calls[0][0], 'legacy', path);
    assert.equal(e.calls[0][1], request);
    assert.equal(result, e.legacyResponse);
  }
});
