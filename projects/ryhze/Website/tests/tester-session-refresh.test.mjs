import test from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { attachDocumentSession, safeReturnPath } from '../auth/document-session.mjs';
import { bootSessionGuard } from '../auth/ui/session-guard.mjs';
const origin = 'https://testers.example.test';
const config = {
  origin,
  issuer: 'https://clerk.example.test',
  publishableKey: 'pk_live_SYNTHETIC',
};
test('every approved HTML response receives pinned SDK refresh without modifying non-HTML assets', async () => {
  const response = await attachDocumentSession(
    new Response(
      '<!doctype html><html><head><title>Private</title></head><body>PRIVATE</body></html>',
      { headers: { 'Content-Type': 'text/html', 'Content-Length': '5' } },
    ),
    config,
    '/downloads/',
  );
  const html = await response.text();
  assert.match(html, /src="\/__auth\/clerk.js"/);
  assert.match(html, /bootSessionGuard/);
  assert.match(html, /getToken/);
  assert.match(html, /"returnTo":"\/downloads\/"/);
  assert.match(response.headers.get('Content-Security-Policy'), /font-src 'self'/);
  assert.equal(response.headers.get('content-length'), null);
  const image = new Response('private image', { headers: { 'Content-Type': 'image/png' } });
  assert.equal(await attachDocumentSession(image, config, '/image.png'), image);
  assert.equal(safeReturnPath('https://evil.example.test/'), '/');
  assert.equal(safeReturnPath('//evil.example.test/'), '/');
  assert.equal(safeReturnPath('/downloads/'), '/downloads/');
});
function fixture(
  t,
  { status = 204, signedIn = true, throwLoad = false, reviewingAgreements = false } = {},
) {
  const old = {};
  for (const key of ['document', 'window', 'fetch', 'setInterval', 'clearInterval'])
    old[key] = globalThis[key];
  const { document } = parseHTML('<html><body><main>PRIVATE CONTENT</main></body></html>');
  const state = { token: 0, status, interval: null, listener: null, cleared: false };
  globalThis.document = document;
  const handlers = {};
  const clerk = {
    load: async () => {
      if (throwLoad) throw new Error('Unavailable');
    },
    session: signedIn
      ? {
          getToken: async (options) => {
            assert.equal(options.skipCache, true);
            state.token++;
            return 'fresh-' + state.token;
          },
        }
      : null,
    addListener: (listener) => {
      state.listener = listener;
    },
  };
  globalThis.window = {
    Clerk: clerk,
    addEventListener: (name, listener) => {
      handlers[name] = listener;
    },
  };
  globalThis.fetch = async (url, options) => {
    assert.equal(url, reviewingAgreements ? '/__auth/session' : '/__auth/content-status');
    assert.equal(options.headers.Authorization, 'Bearer fresh-' + state.token);
    return new Response(null, { status: state.status });
  };
  globalThis.setInterval = (callback, period) => {
    assert.equal(period, 30000);
    state.interval = callback;
    return 1;
  };
  globalThis.clearInterval = () => {
    state.cleared = true;
  };
  t.after(() => {
    for (const key of Object.keys(old)) {
      if (old[key] === undefined) delete globalThis[key];
      else globalThis[key] = old[key];
    }
  });
  return {
    state,
    document,
    handlers,
    clerk,
    boot: () =>
      bootSessionGuard({
        ...config,
        nonce: 'synthetic',
        returnTo: '/downloads/',
        reviewingAgreements,
      }),
  };
}
test('session guard refreshes immediately, periodically and on browser resume, then locks on revocation', async (t) => {
  const f = fixture(t);
  await f.boot();
  assert.equal(f.state.token, 1);
  assert.match(f.document.body.textContent, /PRIVATE CONTENT/);
  await f.state.interval();
  assert.equal(f.state.token, 2);
  await f.handlers.pageshow();
  assert.equal(f.state.token, 3);
  f.state.status = 403;
  await f.handlers.focus();
  assert.match(f.document.body.textContent, /Private preview locked/);
  assert.doesNotMatch(f.document.body.textContent, /PRIVATE CONTENT/);
  assert.equal(f.state.cleared, true);
  assert.equal(
    f.document.querySelector('a').getAttribute('href'),
    '/__auth/sign-in?returnTo=%2Fdownloads%2F',
  );
});
test('session guard responds to provider sign-out without waiting for another page navigation', async (t) => {
  const f = fixture(t);
  await f.boot();
  f.state.listener({ session: null });
  assert.match(f.document.body.textContent, /session ended/);
  assert.doesNotMatch(f.document.body.textContent, /PRIVATE CONTENT/);
});
test('missing SDK/session or network failure never treats old rendered content as fresh authorization', async (t) => {
  const f = fixture(t, { signedIn: false });
  await f.boot();
  assert.match(f.document.body.textContent, /Private preview locked/);
  assert.equal(f.state.interval, null);
});
test('legal review maintains session and membership without demanding prior legal acceptance', async (t) => {
  const f = fixture(t, { status: 200, reviewingAgreements: true });
  await f.boot();
  assert.match(f.document.body.textContent, /PRIVATE CONTENT/);
  assert.equal(f.state.token, 1);
  f.state.status = 403;
  await f.state.interval();
  assert.match(f.document.body.textContent, /Private preview locked/);
});
