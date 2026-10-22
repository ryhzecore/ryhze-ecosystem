import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import pkg from '../../../package.json' with { type: 'json' };
import publicPaths from '../paths.json' with { type: 'json' };
import worker from '../worker.mjs';

const aliases = ['/race/architecture', '/race/architecture/', '/race/architecture/index.html'];
const html = readFileSync(
  new URL('../../../dist/race/architecture/index.html', import.meta.url),
  'utf8',
);

function bindings() {
  const calls = [];
  const legacyResponse = new Response('existing backend response', {
    status: 404,
    headers: {
      'Content-Type': 'text/html',
      'Set-Cookie': 'session=synthetic; HttpOnly',
      'Cache-Control': 'private, no-store',
    },
  });
  return {
    calls,
    legacyResponse,
    ASSETS: {
      async fetch(request) {
        calls.push(['assets', request]);
        return new Response(html, { headers: { 'Content-Type': 'text/html; charset=utf-8' } });
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

test('new architecture page has all three exact generated public route aliases', () => {
  for (const path of aliases) assert.ok(publicPaths.includes(path), path);
});

test('architecture GET and HEAD use the built page on both hosts while preserving the original query', async () => {
  const query = '?view=core&return=https%3A%2F%2Fexample.com%2F&item=1&item=2';
  for (const scheme of ['http', 'https']) {
    for (const host of ['ryhze.com', 'www.ryhze.com']) {
      for (const path of aliases) {
        for (const method of ['GET', 'HEAD']) {
          const env = bindings();
          const request = new Request(`${scheme}://${host}${path}${query}`, { method });
          const response = await worker.fetch(request, env);
          assert.deepEqual(env.calls, [['assets', request]], request.url);
          assert.equal(new URL(env.calls[0][1].url).search, query);
          assert.equal(response.status, 200);
          assert.equal(response.headers.get('Location'), null);
          assert.equal(response.headers.get('Cache-Control'), 'public, max-age=0, must-revalidate');
          assert.equal(await response.text(), method === 'HEAD' ? '' : html);
        }
      }
    }
  }
});

test('architecture page retains frontend security headers and the active website release identity', async () => {
  const response = await worker.fetch(
    new Request('https://ryhze.com/race/architecture/'),
    bindings(),
  );
  for (const [name, value] of Object.entries({
    'X-Ryhze-Release': pkg.version,
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'strict-origin-when-cross-origin',
    'X-Frame-Options': 'DENY',
    'X-Robots-Tag': 'noindex, nofollow, noarchive',
    'Permissions-Policy': 'camera=(), microphone=(), geolocation=()',
  }))
    assert.equal(response.headers.get(name), value, name);
  assert.equal(pkg.version, '0.0.5');
  for (const name of [
    'Set-Cookie',
    'CDN-Cache-Control',
    'Cloudflare-CDN-Cache-Control',
    'Surrogate-Control',
  ])
    assert.equal(response.headers.get(name), null, name);
});

test('credential-bearing public architecture reads remain private and no-store', async () => {
  for (const credential of [
    'Cookie',
    'Authorization',
    'CF-Access-Jwt-Assertion',
    'CF-Access-Authenticated-User-Email',
  ]) {
    for (const method of ['GET', 'HEAD']) {
      for (const path of aliases) {
        const env = bindings();
        const request = new Request(`https://www.ryhze.com${path}`, {
          method,
          headers: { [credential]: 'synthetic-value' },
        });
        const response = await worker.fetch(request, env);
        assert.deepEqual(env.calls, [['assets', request]]);
        assert.equal(response.status, 200);
        assert.equal(response.headers.get('Cache-Control'), 'private, no-store');
        assert.equal(response.headers.get(credential), null);
        assert.equal(response.headers.get('Set-Cookie'), null);
        assert.equal(await response.text(), method === 'HEAD' ? '' : html);
      }
    }
  }
});

test('architecture non-read requests preserve the exact legacy request, body and response', async () => {
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']) {
    for (const path of aliases) {
      const env = bindings();
      const request = new Request(`https://ryhze.com${path}?keep=1`, {
        method,
        body: 'original architecture request',
        headers: { Cookie: 'session=synthetic' },
      });
      assert.equal(await worker.fetch(request, env), env.legacyResponse);
      assert.deepEqual(env.calls, [['legacy', request]]);
      assert.equal(await env.calls[0][1].text(), 'original architecture request');
    }
  }
});

test('architecture near misses and private descendants preserve existing legacy responses', async () => {
  for (const path of [
    '/race/architecture/unknown/',
    '/race/architecture.json',
    '/race/architectures/',
    '/race/Architecture/',
    '/race/%61rchitecture/',
    '/race//architecture/',
    '/race/architecture/index.html/extra',
    '/race/architecture/api/',
    '/race/architecture/private/',
    '/api/race/architecture/',
    '/auth/race/architecture/',
  ]) {
    const env = bindings();
    const request = new Request(`https://ryhze.com${path}`, {
      headers: { Accept: 'text/html', Cookie: 'session=synthetic' },
    });
    assert.equal(await worker.fetch(request, env), env.legacyResponse, path);
    assert.deepEqual(env.calls, [['legacy', request]], path);
  }
});

test('unrelated hosts cannot reach the architecture page through either binding', async () => {
  for (const host of ['video.ryhze.com', 'example.com', 'ryhze.com.example.com']) {
    const env = bindings();
    const response = await worker.fetch(new Request(`https://${host}/race/architecture/`), env);
    assert.equal(response.status, 404);
    assert.equal(response.headers.get('Location'), null);
    assert.deepEqual(env.calls, []);
  }
});
