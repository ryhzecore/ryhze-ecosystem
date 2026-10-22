import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

// Use Wrangler's already locked runtime dependency. No external service, real
// account, owner session, or mocked identityFetch is used in these tests.
const require = createRequire(import.meta.url);
const wranglerRequire = createRequire(require.resolve('wrangler/package.json'));
const { Miniflare } = await import(pathToFileURL(wranglerRequire.resolve('miniflare')).href);
const method = { id: '00000000-0000-4000-8000-000000000001', type: 'onetimepin' };
const issuer = 'https://synthetic.example.test';
const endpoint = `${issuer}/cdn-cgi/access/get-identity`;
const source = await readFile(
  new URL('../hosting/access-login-method.mjs', import.meta.url),
  'utf8',
);
const diagnostics = await readFile(
  new URL('../hosting/access-denial-diagnostics.mjs', import.meta.url),
  'utf8',
);
const worker = `
  import {createLoginMethodVerifier} from './access-login-method.mjs';
  const verify=createLoginMethodVerifier({issuer:${JSON.stringify(issuer)},methods:[${JSON.stringify(method)}]});
  export default {async fetch(){
    const codes=[];
    const result=await verify({token:'synthetic-token',payload:{type:'app',identity_nonce:'synthetic-nonce',sub:'synthetic-owner',email:'owner@example.test'},onFailure:c=>codes.push(c)});
    return Response.json({result,codes});
  }};
`;
function runtime(verifierSource, handler) {
  return new Miniflare({
    cf: false,
    telemetry: { enabled: false },
    workers: [
      {
        config: {
          name: 'local-access-transport-regression',
          compatibilityDate: '2026-10-01',
          manifest: {
            mainModule: 'worker.mjs',
            modules: {
              'worker.mjs': { type: 'esm', contents: worker },
              'access-login-method.mjs': { type: 'esm', contents: verifierSource },
              'access-denial-diagnostics.mjs': { type: 'esm', contents: diagnostics },
            },
          },
        },
        // Intercept the outbound network after Workerd has processed the actual
        // fetch/Request options. This is not an application transport injection.
        dev: { outboundService: { type: 'fetcher', handler } },
      },
    ],
  });
}

test('actual Workerd reproduces the old redirect:error failure before any network request', async () => {
  assert.match(source, /redirect: 'manual'/);
  let calls = 0;
  const previous = source.replace("redirect: 'manual'", "redirect: 'error'");
  const mf = runtime(previous, async () => {
    calls += 1;
    return new Response('unused');
  });
  try {
    const result = await (await mf.dispatchFetch('http://localhost/')).json();
    assert.deepEqual(result, { result: false, codes: ['RZ-A05'] });
    assert.equal(calls, 0);
  } finally {
    await mf.dispose();
  }
});

test('actual Workerd fetch accepts the valid identity and never follows redirect responses', async () => {
  let status = 200;
  let destination = 'https://never-follow.example.test/';
  let calls = [];
  const mf = runtime(source, async (request) => {
    calls.push({ url: request.url, cookie: request.headers.get('Cookie') });
    if (status !== 200) return new Response(null, { status, headers: { Location: destination } });
    return Response.json({
      user_uuid: 'synthetic-owner',
      email: 'owner@example.test',
      idp: method,
    });
  });
  try {
    const accepted = await (await mf.dispatchFetch('http://localhost/')).json();
    assert.deepEqual(accepted, { result: true, codes: [] });
    assert.deepEqual(calls, [{ url: endpoint, cookie: 'CF_Authorization=synthetic-token' }]);
    for (const redirectStatus of [301, 302, 303, 307, 308]) {
      for (const target of [`${issuer}/another-path`, 'https://never-follow.example.test/']) {
        status = redirectStatus;
        destination = target;
        calls = [];
        const denied = await (await mf.dispatchFetch('http://localhost/')).json();
        assert.deepEqual(denied, { result: false, codes: ['RZ-A07'] });
        assert.deepEqual(calls, [{ url: endpoint, cookie: 'CF_Authorization=synthetic-token' }]);
      }
    }
  } finally {
    await mf.dispose();
  }
});
