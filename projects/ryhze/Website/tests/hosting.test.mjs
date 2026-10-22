import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { assertSafePreparation } from '../hosting/preflight.mjs';
import { privateResponse, unavailable } from '../hosting/safety.mjs';

const config = JSON.parse(readFileSync(new URL('../wrangler.json', import.meta.url), 'utf8'));

test('staging configuration has no public URL, live routes, or old application bindings', () => {
  assert.doesNotThrow(() => assertSafePreparation(config));
});
for (const [key, value] of Object.entries({
  workers_dev: true,
  preview_urls: true,
  routes: ['ryhze.com/*'],
  account_id: 'old-account',
  d1_databases: [{}],
  r2_buckets: [{}],
  services: [{}],
  env: { production: {} },
  access: { dev: {} },
})) {
  test(`preflight rejects ${key} that could expose or cross-bind the preview`, () => {
    assert.throws(() => assertSafePreparation({ ...config, [key]: value }));
  });
}
test('asset-first routing is rejected even for a subset of assets', () => {
  for (const route of [false, ['/api/*'], undefined]) {
    assert.throws(() =>
      assertSafePreparation({ ...config, assets: { ...config.assets, run_worker_first: route } }),
    );
  }
});
test('private-response wrapper strips validators/public caching and keeps content', async () => {
  const response = privateResponse(
    new Response('protected', {
      headers: {
        'Cache-Control': 'public, max-age=31536000',
        ETag: 'secret-etag',
        'Last-Modified': 'yesterday',
        'Access-Control-Allow-Origin': '*',
      },
    }),
  );
  assert.equal(await response.text(), 'protected');
  assert.equal(response.headers.get('cache-control'), 'private, no-store, max-age=0');
  assert.equal(response.headers.get('cdn-cache-control'), 'no-store');
  assert.equal(response.headers.get('cloudflare-cdn-cache-control'), 'no-store');
  assert.equal(response.headers.get('vary'), '*');
  for (const key of ['etag', 'last-modified', 'access-control-allow-origin'])
    assert.equal(response.headers.get(key), null);
  assert.match(response.headers.get('x-robots-tag'), /noindex/);
});
test('unavailable response is generic, bodyless for HEAD, and never cached', async () => {
  const response = unavailable('HEAD');
  assert.equal(response.status, 503);
  assert.equal(await response.text(), '');
  assert.match(response.headers.get('cache-control'), /no-store/);
});

test('checked-in Worker refuses every route without invoking static assets', async () => {
  const { default: worker } = await import('../hosting/worker.mjs');
  let fetched = 0;
  const env = {
    ASSETS: {
      fetch() {
        fetched += 1;
        throw new Error('Asset leak');
      },
    },
  };
  for (const path of [
    '/',
    '/_astro/site.js',
    '/fonts/inter.woff2',
    '/brand/logo.png',
    '/version.json',
    '/legal/',
    '/__gate/receipt',
    '/404.html',
  ]) {
    for (const method of ['GET', 'HEAD', 'POST', 'OPTIONS']) {
      const response = await worker.fetch(
        new Request(`https://private.example.test${path}`, { method }),
        env,
      );
      assert.equal(response.status, 503);
      assert.match(response.headers.get('cache-control'), /no-store/);
      if (method === 'HEAD') assert.equal(await response.text(), '');
    }
  }
  assert.equal(fetched, 0);
});
test('strict gate Content Security Policy is preserved by the host wrapper', () => {
  const policy = "default-src 'none'; form-action 'self'";
  const response = privateResponse(
    new Response('gate', { headers: { 'Content-Security-Policy': policy } }),
  );
  assert.equal(response.headers.get('content-security-policy'), policy);
});
