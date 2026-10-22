import assert from 'node:assert/strict';
import test from 'node:test';
import { createLocalJWKSet, exportJWK, generateKeyPair, SignJWT } from 'jose';
import { createAccessIdentity, validateAccessConfig } from '../hosting/access-identity.mjs';

const now = 1790838000;
const config = {
  issuer: 'https://example.cloudflareaccess.com',
  audience: 'test-only-audience',
  allowedEmails: ['owner@example.test'],
};
const { publicKey, privateKey } = await generateKeyPair('RS256');
const jwk = { ...(await exportJWK(publicKey)), kid: 'test-only' };
const identity = createAccessIdentity(config, {
  keyResolver: createLocalJWKSet({ keys: [jwk] }),
  clock: () => new Date(now * 1000),
});
async function token(overrides = {}, key = privateKey) {
  return new SignJWT({
    iss: config.issuer,
    aud: config.audience,
    sub: 'test-user',
    email: 'owner@example.test',
    iat: now - 10,
    exp: now + 3600,
    ...overrides,
  })
    .setProtectedHeader({ alg: 'RS256', kid: 'test-only' })
    .sign(key);
}
function request(jwt, extra = {}) {
  return new Request('https://private.example.test/', {
    headers: { ...(jwt ? { 'Cf-Access-Jwt-Assertion': jwt } : {}), ...extra },
  });
}
test('a valid approved assertion yields stable pseudonymous identity and session references', async () => {
  const jwt = await token();
  const first = await identity.resolve(request(jwt));
  assert.match(first.subjectRef, /^[a-f0-9]{64}$/);
  assert.match(first.sessionRef, /^[a-f0-9]{64}$/);
  assert.deepEqual(first, await identity.resolve(request(jwt)));
  assert.ok(!JSON.stringify(first).includes('owner@example'));
  const refreshed = await identity.resolve(request(await token({ iat: now - 5 })));
  assert.equal(refreshed.subjectRef, first.subjectRef);
  assert.notEqual(refreshed.sessionRef, first.sessionRef);
});
for (const [name, claims] of Object.entries({
  issuer: { iss: 'https://attacker.example' },
  audience: { aud: 'another-app' },
  expired: { exp: now - 1 },
  future: { nbf: now + 1 },
  futureIssued: { iat: now + 1 },
  noSubject: { sub: '' },
  noEmail: { email: undefined },
  unapproved: { email: 'other@example.test' },
  noExpiry: { exp: undefined },
  noIssueTime: { iat: undefined },
})) {
  test(`rejects ${name} even with a correctly signed JWT`, async () => {
    assert.equal(await identity.resolve(request(await token(claims))), null);
  });
}
test('plain email headers, cookies, malformed tokens, and excessive tokens do not authenticate', async () => {
  for (const value of [null, 'not.a.token', 'x'.repeat(16385)]) {
    assert.equal(
      await identity.resolve(
        request(value, {
          'Cf-Access-Authenticated-User-Email': 'owner@example.test',
          Cookie: 'accepted=true',
        }),
      ),
      null,
    );
  }
});
test('a signature from a different key is denied', async () => {
  const other = await generateKeyPair('RS256');
  assert.equal(await identity.resolve(request(await token({}, other.privateKey))), null);
});
test('token expiry is rechecked after asynchronous key resolution', async () => {
  let current = now;
  const delayed = createAccessIdentity(config, {
    keyResolver: async () => {
      current = now + 3601;
      return publicKey;
    },
    clock: () => new Date(current * 1000),
  });
  assert.equal(await delayed.resolve(request(await token())), null);
});
test('untrusted issuers or an empty allowlist cannot configure the verifier', () => {
  for (const issuer of [
    'http://example.cloudflareaccess.com',
    'https://example.cloudflareaccess.com.evil.test',
    'https://evil.test',
    'https://example.cloudflareaccess.com/path',
    'https://user@example.cloudflareaccess.com',
    'https://example.cloudflareaccess.com/',
  ]) {
    assert.throws(() => validateAccessConfig({ ...config, issuer }));
  }
  assert.throws(() => validateAccessConfig({ ...config, audience: '' }));
  assert.throws(() => validateAccessConfig({ ...config, allowedEmails: [] }));
});
