import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import pkg from '../../../package.json' with { type: 'json' };
import worker from '../worker.mjs';
function bindings() {
  const calls = [];
  const legacyResponse = new Response('legacy', {
    headers: { 'Set-Cookie': 'synthetic=value; HttpOnly', 'Cache-Control': 'private' },
  });
  return {
    calls,
    legacyResponse,
    ASSETS: {
      async fetch(req) {
        calls.push(['assets', req]);
        return new Response('public');
      },
    },
    LEGACY: {
      async fetch(req) {
        calls.push(['legacy', req]);
        return legacyResponse;
      },
    },
  };
}
test('exact marketing pages with query strings use new assets on both hosts', async () => {
  for (const host of ['ryhze.com', 'www.ryhze.com'])
    for (const path of [
      '/',
      '/?campaign=test',
      '/entertainment',
      '/entertainment/?view=films',
      '/games/',
      '/films/',
      '/legal/',
      '/privacy/',
    ]) {
      const e = bindings();
      const req = new Request('https://' + host + path);
      const r = await worker.fetch(req, e);
      assert.equal(e.calls[0][0], 'assets');
      assert.equal(e.calls[0][1], req);
      assert.equal(await r.text(), 'public');
    }
});
test('every unknown path falls through to old Worker unchanged', async () => {
  for (const path of [
    '/api',
    '/api/auth',
    '/portal',
    '/media/file',
    '/video/clip',
    '/unknown',
    '/_astro/legacy-file.js',
    '/brand/legacy-file.png',
    '/__auth',
    '/%61pi',
    '//portal',
  ]) {
    const e = bindings();
    const req = new Request('https://ryhze.com' + path, {
      headers: { Cookie: 'synthetic=value', 'X-Test': 'preserve' },
    });
    const r = await worker.fetch(req, e);
    assert.equal(e.calls[0][0], 'legacy', path);
    assert.equal(e.calls[0][1], req);
    assert.equal(r, e.legacyResponse);
    assert.equal(r.headers.get('Cache-Control'), 'private');
    assert.ok(r.headers.has('Set-Cookie'));
  }
});
test('write and preflight methods are passed through, even on marketing paths', async () => {
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']) {
    const e = bindings();
    const req = new Request('https://ryhze.com/entertainment/', { method, body: 'synthetic body' });
    await worker.fetch(req, e);
    assert.equal(e.calls[0][0], 'legacy');
    assert.equal(e.calls[0][1], req);
    assert.equal(await e.calls[0][1].text(), 'synthetic body');
  }
});
test('new static HEAD has no body and carries frontend headers', async () => {
  const e = bindings();
  const r = await worker.fetch(new Request('https://ryhze.com/', { method: 'HEAD' }), e);
  assert.equal(await r.text(), '');
  assert.equal(r.headers.get('X-Ryhze-Release'), pkg.version);
});
test('video and unrelated hostnames cannot reach this wrapper', async () => {
  for (const host of [
    'video.ryhze.com',
    'portal-origin.ryhze.com',
    'ryhze-web.live-insights.workers.dev',
    'example.com',
  ]) {
    const e = bindings();
    const r = await worker.fetch(new Request('https://' + host + '/'), e);
    assert.equal(r.status, 404);
    assert.equal(e.calls.length, 0);
  }
});

test('legacy-only rollback returns exact upstream responses for public and legacy requests', async () => {
  const rollback = (await import('../legacy-only.mjs')).default;
  for (const path of ['/', '/entertainment/?view=films', '/games/', '/any-legacy-path']) {
    for (const method of ['GET', 'HEAD', 'POST']) {
      const e = bindings();
      const request = new Request('https://ryhze.com' + path, { method });
      const result = await rollback.fetch(request, e);
      assert.equal(e.calls[0][0], 'legacy');
      assert.equal(e.calls[0][1], request);
      assert.equal(result, e.legacyResponse);
    }
  }
});

test('RACE and availability routes match slash, no-slash and index forms', async () => {
  for (const name of ['race', 'library', 'film-library', 'saved']) {
    for (const suffix of ['', '/', '/index.html']) {
      for (const method of ['GET', 'HEAD']) {
        const e = bindings();
        await worker.fetch(
          new Request(`https://ryhze.com/${name}${suffix}?check=1`, { method }),
          e,
        );
        assert.equal(e.calls[0][0], 'assets', name + suffix);
      }
    }
  }
});

function missingBindings(status = 404, headers = {}) {
  const calls = [];
  const legacyResponse = new Response('legacy missing', {
    status,
    headers: { 'Content-Type': 'text/html', ...headers },
  });
  return {
    calls,
    legacyResponse,
    LEGACY: {
      async fetch(req) {
        calls.push(['legacy', req]);
        return legacyResponse;
      },
    },
    ASSETS: {
      async fetch(req) {
        calls.push(['assets', req]);
        return new Response('current 404', { headers: { 'Content-Type': 'text/html' } });
      },
    },
  };
}
test('anonymous missing HTML navigations use current 404 without redirecting the requested URL', async () => {
  for (const method of ['GET', 'HEAD']) {
    for (const path of ['/missing', '/missing/', '/nested/missing']) {
      const e = missingBindings();
      const req = new Request(`https://ryhze.com${path}?retain=1`, {
        method,
        headers: { Accept: 'text/html,application/xhtml+xml' },
      });
      const r = await worker.fetch(req, e);
      assert.equal(e.calls[0][1], req);
      assert.equal(new URL(e.calls[1][1].url).pathname, '/404.html');
      assert.equal(r.status, 404);
      assert.equal(r.headers.get('Cache-Control'), 'no-store');
      assert.equal(r.headers.get('Location'), null);
      assert.equal(await r.text(), method === 'HEAD' ? '' : 'current 404');
    }
  }
});
test('fallback preserves backend, media, asset, ambiguous and authenticated requests exactly', async () => {
  for (const path of [
    '/api/missing',
    '/account/missing',
    '/login',
    '/__auth',
    '/media/missing',
    '/video/clip',
    '/assets/missing',
    '/images/missing',
    '/fonts/missing',
    '/brand/missing',
    '/_astro/missing',
    '/file.js',
    '/%61pi',
    '//portal',
  ]) {
    const e = missingBindings();
    const req = new Request('https://ryhze.com' + path, { headers: { Accept: 'text/html' } });
    assert.equal(await worker.fetch(req, e), e.legacyResponse, path);
    assert.equal(e.calls.length, 1);
    assert.equal(e.calls[0][1], req);
  }
  for (const headers of [
    { Authorization: 'Bearer synthetic' },
    { 'CF-Access-Jwt-Assertion': 'synthetic' },
    { 'CF-Access-Authenticated-User-Email': 'synthetic@example.com' },
    { Accept: 'application/json' },
  ]) {
    const e = missingBindings();
    const req = new Request('https://ryhze.com/missing', {
      headers: { Accept: 'text/html', ...headers },
    });
    assert.equal(await worker.fetch(req, e), e.legacyResponse);
    assert.equal(e.calls.length, 1);
  }
});
test('fallback never replaces successful legacy shells, redirects, challenges or cookie-setting errors', async () => {
  for (const [status, headers] of [
    [200, {}],
    [302, { Location: '/login' }],
    [401, {}],
    [500, {}],
    [404, { 'Set-Cookie': 'session=synthetic' }],
    [404, { 'WWW-Authenticate': 'Bearer' }],
    [404, { 'Content-Type': 'application/json' }],
  ]) {
    const e = missingBindings(status, headers);
    const req = new Request('https://ryhze.com/missing', { headers: { Accept: 'text/html' } });
    assert.equal(await worker.fetch(req, e), e.legacyResponse);
    assert.equal(e.calls.length, 1);
  }
});

test('ordinary cookies do not keep an old missing public page and remain private no-store', async () => {
  for (const method of ['GET', 'HEAD']) {
    for (const path of ['/something', '/something/', '/extra/subpage']) {
      const e = missingBindings();
      const req = new Request('https://ryhze.com' + path, {
        method,
        headers: { Accept: 'text/html', Cookie: 'session=synthetic; preference=dark' },
      });
      const response = await worker.fetch(req, e);
      assert.equal(e.calls[0][1], req, 'original backend request and cookie are preserved');
      assert.equal(e.calls[0][1].headers.get('Cookie'), 'session=synthetic; preference=dark');
      assert.equal(new URL(e.calls[1][1].url).pathname, '/404.html');
      assert.equal(
        e.calls[1][1].headers.get('Cookie'),
        null,
        'static error fetch does not transmit cookies',
      );
      assert.equal(response.status, 404);
      assert.equal(response.headers.get('Cache-Control'), 'private, no-store');
      assert.equal(response.headers.get('Set-Cookie'), null);
      assert.equal(await response.text(), method === 'HEAD' ? '' : 'current 404');
    }
  }
});
test('cookie-bearing protected and non-read requests retain the exact legacy response', async () => {
  for (const path of [
    '/api/missing',
    '/account/missing',
    '/private/missing',
    '/portal/missing',
    '/auth/missing',
    '/__auth',
    '/__gate',
    '/__agreements',
    '/login',
    '/logout',
    '/oauth/callback',
    '/session/missing',
    '/media/missing',
    '/video/clip',
    '/assets/missing',
    '/_astro/missing.js',
  ]) {
    const e = missingBindings();
    const req = new Request('https://ryhze.com' + path, {
      headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
    });
    assert.equal(await worker.fetch(req, e), e.legacyResponse, path);
    assert.equal(e.calls.length, 1);
    assert.equal(e.calls[0][1], req);
  }
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']) {
    const e = missingBindings();
    const req = new Request('https://ryhze.com/something', {
      method,
      body: 'original',
      headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
    });
    assert.equal(await worker.fetch(req, e), e.legacyResponse);
    assert.equal(e.calls.length, 1);
    assert.equal(await e.calls[0][1].text(), 'original');
  }
});
test('cookie-bearing non-404, challenge, Set-Cookie and explicit authorization responses remain unchanged', async () => {
  for (const [status, headers] of [
    [200, {}],
    [302, { Location: '/login' }],
    [401, {}],
    [500, {}],
    [404, { 'Set-Cookie': 'session=refreshed' }],
    [404, { 'WWW-Authenticate': 'Bearer' }],
  ]) {
    const e = missingBindings(status, headers);
    const req = new Request('https://ryhze.com/something', {
      headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
    });
    assert.equal(await worker.fetch(req, e), e.legacyResponse);
    assert.equal(e.calls.length, 1);
  }
  for (const header of [
    'Authorization',
    'CF-Access-Jwt-Assertion',
    'CF-Access-Authenticated-User-Email',
  ]) {
    const e = missingBindings();
    const req = new Request('https://ryhze.com/something', {
      headers: { Accept: 'text/html', Cookie: 'session=synthetic', [header]: 'synthetic' },
    });
    assert.equal(await worker.fetch(req, e), e.legacyResponse);
    assert.equal(e.calls.length, 1);
  }
});

test('frontend release header agrees with package and both built version endpoints', async () => {
  const version = JSON.parse(
    readFileSync(new URL('../../../dist/version.json', import.meta.url), 'utf8'),
  );
  const buildInfo = JSON.parse(
    readFileSync(new URL('../../../dist/build-info.json', import.meta.url), 'utf8'),
  );
  assert.equal(version.version, pkg.version);
  assert.equal(version.version, '0.0.5');
  assert.equal(version.stage, 'PRE-ALPHA');
  assert.equal(version.channel, 'development');
  assert.deepEqual(buildInfo, version);
  for (const host of ['ryhze.com', 'www.ryhze.com']) {
    for (const method of ['GET', 'HEAD']) {
      for (const path of [
        '/',
        '/entertainment/',
        '/race/',
        '/race/architecture/',
        '/version.json',
        '/build-info.json',
      ]) {
        const e = bindings();
        const response = await worker.fetch(new Request(`https://${host}${path}`, { method }), e);
        assert.equal(response.headers.get('X-Ryhze-Release'), version.version);
      }
      const e = bindings();
      const response = await worker.fetch(
        new Request(`https://${host}/race/progress/scene/?keep=1`, { method }),
        e,
      );
      assert.equal(response.status, 302);
      assert.equal(response.headers.get('X-Ryhze-Release'), version.version);
      const missing = await worker.fetch(
        new Request(`https://${host}/something`, {
          method,
          headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
        }),
        missingBindings(),
      );
      assert.equal(missing.status, 404);
      assert.equal(missing.headers.get('X-Ryhze-Release'), version.version);
    }
  }
});
