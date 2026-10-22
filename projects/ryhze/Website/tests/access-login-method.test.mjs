import assert from 'node:assert/strict';
import test from 'node:test';
import { createLocalJWKSet, exportJWK, generateKeyPair, SignJWT } from 'jose';
import { createAccessIdentity } from '../hosting/access-identity.mjs';
import { validateLoginMethods } from '../hosting/access-login-method.mjs';

const now = 1790838000;
const config = {
  issuer: 'https://synthetic.cloudflareaccess.com',
  audience: 'synthetic-audience',
  allowedEmails: ['owner@example.test'],
};
const methods = [
  { id: '00000000-0000-4000-8000-000000000001', type: 'google' },
  { id: '00000000-0000-4000-8000-000000000002', type: 'onetimepin' },
];
const identity = {
  user_uuid: 'synthetic-owner',
  email: 'owner@example.test',
  idp: methods[0],
  service_token_status: false,
};
const { publicKey, privateKey } = await generateKeyPair('RS256');
const keyResolver = createLocalJWKSet({ keys: [{ ...(await exportJWK(publicKey)), kid: 'test' }] });
async function token(changes = {}) {
  return new SignJWT({
    iss: config.issuer,
    aud: config.audience,
    sub: identity.user_uuid,
    email: identity.email,
    type: 'app',
    identity_nonce: 'synthetic-nonce',
    iat: now - 10,
    exp: now + 3600,
    ...changes,
  })
    .setProtectedHeader({ alg: 'RS256', kid: 'test' })
    .sign(privateKey);
}
function request(jwt) {
  return new Request('https://review.example.test/?provider=google', {
    headers: {
      'cf-access-jwt-assertion': jwt,
      Cookie: 'CF_Authorization=untrusted; tracking=unrelated',
      Authorization: 'Bearer untrusted',
      'x-auth-provider': 'google',
    },
  });
}
function verifier(identityFetch, extra = {}) {
  return createAccessIdentity(config, {
    keyResolver,
    clock: () => new Date(now * 1000),
    requireApplicationToken: true,
    loginMethods: methods,
    identityFetch,
    ...extra,
  });
}
for (const method of methods) {
  test(`accepts only a verified ${method.type} identity tied to the signed owner`, async () => {
    const jwt = await token();
    let calls = 0;
    const result = await verifier(async (url, options) => {
      calls += 1;
      assert.equal(url, `${config.issuer}/cdn-cgi/access/get-identity`);
      assert.equal(options.method, 'GET');
      assert.equal(options.redirect, 'manual');
      assert.equal(options.cache, 'no-store');
      assert.deepEqual(options.headers, {
        Accept: 'application/json',
        Cookie: `CF_Authorization=${jwt}`,
      });
      assert.ok(options.signal instanceof AbortSignal);
      return Response.json({ ...identity, idp: method });
    }).resolve(request(jwt));
    assert.equal(calls, 1);
    assert.match(result.subjectRef, /^[a-f0-9]{64}$/);
    assert.deepEqual(Object.keys(result).sort(), ['sessionRef', 'subjectRef']);
    assert.doesNotMatch(JSON.stringify(result), /owner|google|onetimepin|synthetic/);
  });
}
for (const [name, changes] of Object.entries({
  missingProvider: { idp: undefined },
  differentProviderId: { idp: { ...methods[0], id: '00000000-0000-4000-8000-000000000099' } },
  googleLabelOnOtpId: { idp: { ...methods[1], type: 'google' } },
  otpLabelOnGoogleId: { idp: { ...methods[0], type: 'onetimepin' } },
  unapprovedProviderType: { idp: { ...methods[0], type: 'github' } },
  googleDisplayNameOnly: { idp: { name: 'Google' } },
  missingSubject: { user_uuid: undefined },
  wrongSubject: { user_uuid: 'other-owner' },
  wrongEmail: { email: 'other@example.test' },
  addedEmailAlias: { email: 'owner+review@example.test' },
  missingEmail: { email: undefined },
  serviceToken: { service_token_status: true },
  malformedServiceStatus: { service_token_status: 'false' },
  serviceTokenId: { service_token_id: 'synthetic-service-token' },
})) {
  test(`denies ${name} even when the JWT is correctly signed and owner allowlisted`, async () => {
    assert.equal(
      await verifier(async () => Response.json({ ...identity, ...changes })).resolve(
        request(await token()),
      ),
      null,
    );
  });
}
for (const [name, changes] of Object.entries({
  unapprovedEmail: { email: 'other@example.test' },
  wrongIssuer: { iss: 'https://other.cloudflareaccess.com' },
  wrongAudience: { aud: 'other' },
  expired: { exp: now },
  globalSession: { type: 'org' },
  emptySubject: { sub: '' },
  noNonce: { identity_nonce: undefined },
  emptyNonce: { identity_nonce: '' },
  excessiveNonce: { identity_nonce: 'a'.repeat(257) },
})) {
  test(`does not call an identity endpoint for ${name}`, async () => {
    let calls = 0;
    assert.equal(
      await verifier(async () => {
        calls += 1;
        return Response.json(identity);
      }).resolve(request(await token(changes))),
      null,
    );
    assert.equal(calls, 0);
  });
}
test('a Google-claimed JWT or request cannot bypass a failed provider check', async () => {
  const jwt = await token({ idp: methods[0], amr: ['google'], provider: 'google' });
  assert.equal(
    await verifier(async () =>
      Response.json({ ...identity, idp: { type: 'github', id: methods[0].id } }),
    ).resolve(request(jwt)),
    null,
  );
});
for (const [name, response] of [
  ['redirect', () => Response.redirect('https://outside.example.test')],
  [
    'opaque redirect',
    () => {
      const response = new Response(null);
      Object.defineProperties(response, {
        status: { value: 0 },
        type: { value: 'opaqueredirect' },
      });
      return response;
    },
  ],
  ['denial', () => Response.json(identity, { status: 403 })],
  ['server error', () => Response.json(identity, { status: 503 })],
  ['HTML', () => new Response('<h1>Login</h1>', { headers: { 'Content-Type': 'text/html' } })],
  ['invalid JSON', () => new Response('{', { headers: { 'Content-Type': 'application/json' } })],
  ['null JSON', () => Response.json(null)],
  ['array JSON', () => Response.json([identity])],
  [
    'oversized declared length',
    () => Response.json(identity, { headers: { 'Content-Length': '16385' } }),
  ],
  ['oversized body', () => Response.json({ ...identity, padding: 'x'.repeat(16384) })],
  [
    'unexpected destination',
    () => {
      const response = Response.json(identity);
      Object.defineProperty(response, 'url', { value: 'https://outside.example.test' });
      return response;
    },
  ],
  [
    'network failure',
    () => {
      throw new Error('synthetic network failure');
    },
  ],
]) {
  test(`fails closed on identity endpoint ${name}`, async () => {
    assert.equal(await verifier(async () => response()).resolve(request(await token())), null);
  });
}
test('abort timeout fails closed instead of authorizing on identity lookup failure', async () => {
  const resolve = verifier(
    async (_url, { signal }) =>
      new Promise((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
      }),
  );
  assert.equal(await resolve.resolve(request(await token())), null);
});
test('expiry after identity lookup is denied and identities are never cached', async () => {
  let current = now,
    calls = 0;
  const resolve = verifier(
    async () => {
      calls += 1;
      current = now + 3600;
      return Response.json(identity);
    },
    { clock: () => new Date(current * 1000) },
  );
  assert.equal(await resolve.resolve(request(await token())), null);
  current = now;
  assert.equal(await resolve.resolve(request(await token())), null);
  assert.equal(calls, 2);
});
test('provider list is copied and cannot be broadened by later mutation', async () => {
  const loginMethods = structuredClone(methods);
  const resolve = verifier(
    async () =>
      Response.json({
        ...identity,
        idp: { ...methods[0], id: '00000000-0000-4000-8000-000000000099' },
      }),
    { loginMethods },
  );
  loginMethods[0].id = '00000000-0000-4000-8000-000000000099';
  assert.equal(await resolve.resolve(request(await token())), null);
});
test('empty, duplicate, guessed, unsupported, or malformed providers cannot configure verification', () => {
  for (const invalid of [
    undefined,
    null,
    [],
    {},
    [{}],
    [{ id: 'pending', type: 'google' }],
    [{ ...methods[0], type: 'github' }],
    [methods[0], methods[0]],
    [methods[0], { ...methods[1], type: 'google' }],
    [...methods, methods[0]],
  ])
    assert.throws(() => validateLoginMethods(invalid));
});

test('the complete identity lookup has a hard deadline even if fetch or a body ignores abort', async () => {
  const jwt = await token();
  const outcomes = await Promise.all([
    verifier(async () => new Promise(() => {})).resolve(request(jwt)),
    verifier(
      async () =>
        new Response(new ReadableStream(), { headers: { 'Content-Type': 'application/json' } }),
    ).resolve(request(jwt)),
    verifier(
      async () =>
        new Promise((resolve) => setTimeout(() => resolve(Response.json(identity)), 5100)),
    ).resolve(request(jwt)),
  ]);
  assert.deepEqual(outcomes, [null, null, null]);
});

test('provider IDs differing only by UUID letter case are still duplicates', () => {
  const id = 'abcdefab-cdef-4abc-8def-abcdefabcdef';
  assert.throws(() =>
    validateLoginMethods([
      { id, type: 'onetimepin' },
      { id: id.toUpperCase(), type: 'google' },
    ]),
  );
});
