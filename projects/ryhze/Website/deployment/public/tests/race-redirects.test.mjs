import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import config from '../../../astro.config.mjs';
import pkg from '../../../package.json' with { type: 'json' };
import worker from '../worker.mjs';

const retiredPaths = [
  '/race/progress',
  '/race/progress/',
  '/race/progress/index.html',
  '/race/progress/scene',
  '/race/progress/scene/',
  '/race/progress/scene/index.html',
];

function bindings() {
  const calls = [];
  const legacyResponse = new Response('legacy response', {
    status: 404,
    headers: {
      'Content-Type': 'text/html',
      'Cache-Control': 'private, no-store',
      'Set-Cookie': 'session=synthetic; HttpOnly',
    },
  });
  return {
    calls,
    legacyResponse,
    ASSETS: {
      async fetch(request) {
        calls.push(['assets', request]);
        return new Response('public asset');
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

test('static preview config redirects only the two retired report routes to RACE', () => {
  assert.deepEqual(config.redirects, {
    '/race/progress': { status: 302, destination: '/race/' },
    '/race/progress/scene': { status: 302, destination: '/race/' },
  });
});

test('retired report GET and HEAD aliases redirect on both hosts without consulting bindings', async () => {
  for (const scheme of ['https', 'http']) {
    for (const host of ['ryhze.com', 'www.ryhze.com']) {
      for (const path of retiredPaths) {
        for (const method of ['GET', 'HEAD']) {
          const env = bindings();
          const query = '?campaign=old+report&return=https%3A%2F%2Fexample.com%2F&part=1&part=2';
          const request = new Request(`${scheme}://${host}${path}${query}`, { method });
          const response = await worker.fetch(request, env);
          assert.equal(response.status, 302, request.url);
          assert.equal(response.headers.get('Location'), `${scheme}://${host}/race/${query}`);
          assert.equal(
            new URL(response.headers.get('Location')).origin,
            new URL(request.url).origin,
          );
          assert.equal(response.headers.get('Cache-Control'), 'no-store');
          assert.equal(await response.text(), '');
          assert.deepEqual(env.calls, []);
        }
      }
    }
  }
});

test('retired redirects retain frontend security headers and current release identity', async () => {
  const response = await worker.fetch(new Request('https://ryhze.com/race/progress/'), bindings());
  for (const [name, value] of Object.entries({
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'strict-origin-when-cross-origin',
    'X-Frame-Options': 'DENY',
    'X-Robots-Tag': 'noindex, nofollow, noarchive',
    'Permissions-Policy': 'camera=(), microphone=(), geolocation=()',
    'X-Ryhze-Release': pkg.version,
  })) {
    assert.equal(response.headers.get(name), value, name);
  }
  for (const name of [
    'Set-Cookie',
    'CDN-Cache-Control',
    'Cloudflare-CDN-Cache-Control',
    'Surrogate-Control',
  ]) {
    assert.equal(response.headers.get(name), null);
  }
});

test('credential-bearing public report redirects remain private and never forward credentials', async () => {
  for (const name of [
    'Cookie',
    'Authorization',
    'CF-Access-Jwt-Assertion',
    'CF-Access-Authenticated-User-Email',
  ]) {
    for (const method of ['GET', 'HEAD']) {
      const env = bindings();
      const request = new Request('https://www.ryhze.com/race/progress/scene/', {
        method,
        headers: { [name]: 'synthetic-value' },
      });
      const response = await worker.fetch(request, env);
      assert.equal(response.status, 302);
      assert.equal(response.headers.get('Location'), 'https://www.ryhze.com/race/');
      assert.equal(response.headers.get('Cache-Control'), 'private, no-store');
      assert.equal(response.headers.get(name), null);
      assert.equal(response.headers.get('Set-Cookie'), null);
      assert.deepEqual(env.calls, []);
    }
  }
});

test('non-read report requests preserve the exact legacy request, body and response', async () => {
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']) {
    for (const path of retiredPaths) {
      const env = bindings();
      const request = new Request(`https://ryhze.com${path}?keep=1`, {
        method,
        body: 'original request body',
        headers: { Cookie: 'session=synthetic' },
      });
      assert.equal(await worker.fetch(request, env), env.legacyResponse);
      assert.deepEqual(env.calls, [['legacy', request]]);
      assert.equal(await env.calls[0][1].text(), 'original request body');
    }
  }
});

test('unrelated hosts never receive report redirects or reach either binding', async () => {
  for (const host of ['video.ryhze.com', 'example.com', 'ryhze.com.example.com']) {
    const env = bindings();
    const response = await worker.fetch(new Request(`https://${host}/race/progress/`), env);
    assert.equal(response.status, 404);
    assert.equal(response.headers.get('Location'), null);
    assert.deepEqual(env.calls, []);
  }
});

test('unknown descendants, encoded aliases and private routes keep the legacy fallback intact', async () => {
  for (const path of [
    '/race/progress/unknown/',
    '/race/progress/scene/unknown/',
    '/race/progress/scene.json',
    '/race/progress/index.html/extra',
    '/race/progressive/',
    '/race//progress/',
    '/race/%70rogress/',
    '/race/Progress/',
    '/race/progress/api/',
    '/race/progress/private/',
    '/api/race/progress/',
    '/auth/race/progress/',
    '/__auth/race/progress/',
  ]) {
    const env = bindings();
    const request = new Request(`https://ryhze.com${path}`, {
      headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
    });
    assert.equal(await worker.fetch(request, env), env.legacyResponse, path);
    assert.deepEqual(env.calls, [['legacy', request]], path);
  }
});

test('retired redirects work independently of the generated public asset inventory', async () => {
  // The route is explicit policy, not dependent on old report HTML being built.
  const source = (await readFile(new URL('../worker.mjs', import.meta.url), 'utf8'))
    .replace(/import pkg from [^;]+;/, `const pkg = ${JSON.stringify({ version: pkg.version })};`)
    .replace(/import publicPaths from [^;]+;/, 'const publicPaths = [];')
    .replace(/import cacheManifest from [^;]+;/, 'const cacheManifest = {};');
  const isolated = (
    await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`)
  ).default;
  for (const path of retiredPaths) {
    const env = bindings();
    const response = await isolated.fetch(new Request(`https://ryhze.com${path}`), env);
    assert.equal(response.status, 302);
    assert.equal(response.headers.get('Location'), 'https://ryhze.com/race/');
    assert.deepEqual(env.calls, []);
  }
});
