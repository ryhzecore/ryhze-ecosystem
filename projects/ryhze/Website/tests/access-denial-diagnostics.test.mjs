import assert from 'node:assert/strict';
import test from 'node:test';
import { createLocalJWKSet, exportJWK, generateKeyPair, SignJWT } from 'jose';
import { accessDenialMessage, reportAccessFailure } from '../hosting/access-denial-diagnostics.mjs';
import { createAccessIdentity } from '../hosting/access-identity.mjs';

const now = Date.parse('2026-10-01T09:20:00Z');
const method = { id: '00000000-0000-4000-8000-000000000001', type: 'onetimepin' };
const config = {
  issuer: 'https://synthetic.cloudflareaccess.com',
  audience: 'synthetic-audience',
  allowedEmails: ['owner@example.test'],
};
const { publicKey, privateKey } = await generateKeyPair('RS256');
const keyResolver = createLocalJWKSet({ keys: [{ ...(await exportJWK(publicKey)), kid: 'test' }] });
const validIdentity = {
  user_uuid: 'synthetic-owner',
  email: 'owner@example.test',
  idp: method,
};
async function token(changes = {}) {
  return new SignJWT({
    iss: config.issuer,
    aud: config.audience,
    sub: 'synthetic-owner',
    email: 'owner@example.test',
    type: 'app',
    identity_nonce: 'synthetic-nonce',
    iat: now / 1000 - 10,
    exp: now / 1000 + 3600,
    ...changes,
  })
    .setProtectedHeader({ alg: 'RS256', kid: 'test' })
    .sign(privateKey);
}
function resolver(identityFetch = async () => Response.json(validIdentity)) {
  return createAccessIdentity(config, {
    keyResolver,
    clock: () => new Date(now),
    requireApplicationToken: true,
    loginMethods: [method],
    identityFetch,
  });
}
function request(jwt) {
  return new Request('https://review.example.test/', {
    headers: jwt ? { 'cf-access-jwt-assertion': jwt } : {},
  });
}

test('diagnostics default off and expire without another deployment', () => {
  const enabled = { RYHZE_DENIAL_DIAGNOSTICS_UNTIL: '2026-10-01T09:30:00Z' };
  assert.equal(accessDenialMessage(enabled, 'RZ-A01', now), 'Access denied. Reference: RZ-A01');
  for (const until of [
    undefined,
    '',
    'true',
    '2026-10-01T09:20:00Z',
    '2026-10-01T10:20:00Z',
    '2026-10-01T09:30:00Z?token=secret',
    '2026-99-99T09:30:00Z',
  ]) {
    assert.equal(
      accessDenialMessage({ RYHZE_DENIAL_DIAGNOSTICS_UNTIL: until }, 'RZ-A01', now),
      'Access denied.',
    );
  }
  assert.equal(accessDenialMessage(enabled, 'RZ-A01', now + 600000), 'Access denied.');
  assert.equal(accessDenialMessage(enabled, 'token-or-email', now), 'Access denied.');
  assert.equal(accessDenialMessage({}, 'RZ-A01', now), 'Access denied.');
});

test('reporting emits only finite codes and cannot alter authorization results', () => {
  const seen = [];
  assert.equal(
    reportAccessFailure((x) => seen.push(x), 'RZ-A01', null),
    null,
  );
  assert.equal(
    reportAccessFailure((x) => seen.push(x), 'secret-data', false),
    false,
  );
  assert.deepEqual(seen, ['RZ-A01']);
  assert.equal(
    reportAccessFailure(
      () => {
        throw Error('reporter failure');
      },
      'RZ-A01',
      null,
    ),
    null,
  );
});

for (const [name, jwt, transport, expected] of [
  ['missing assertion', undefined, undefined, 'RZ-A01'],
  ['oversized assertion', 'a'.repeat(16385), undefined, 'RZ-A02'],
  ['invalid JWT', 'invalid', undefined, 'RZ-A03'],
  ['wrong owner', () => token({ email: 'other@example.test' }), undefined, 'RZ-A03'],
  ['missing nonce', () => token({ identity_nonce: undefined }), undefined, 'RZ-A04'],
  [
    'network failure',
    token,
    async () => {
      throw Error('sensitive upstream data');
    },
    'RZ-A05',
  ],
  ['bad identity response', token, async () => new Response('sensitive response'), 'RZ-A07'],
  [
    'identity mismatch',
    token,
    async () => Response.json({ ...validIdentity, user_uuid: 'other' }),
    'RZ-A08',
  ],
  [
    'provider mismatch',
    token,
    async () => Response.json({ ...validIdentity, idp: { ...method, type: 'google' } }),
    'RZ-A09',
  ],
  [
    'service identity',
    token,
    async () => Response.json({ ...validIdentity, service_token_status: true }),
    'RZ-A10',
  ],
]) {
  test(`fixed diagnostic identifies ${name} without changing denial`, async () => {
    const seen = [];
    const value = typeof jwt === 'function' ? await jwt() : jwt;
    assert.equal(await resolver(transport).resolve(request(value), (x) => seen.push(x)), null);
    assert.deepEqual(seen, [expected]);
    assert.doesNotMatch(JSON.stringify(seen), /owner|email|sensitive|nonce|JWT|synthetic/);
    assert.equal(await resolver(transport).resolve(request(value)), null);
  });
}

test('successful authentication does not emit a diagnostic', async () => {
  const seen = [];
  const result = await resolver().resolve(request(await token()), (x) => seen.push(x));
  assert.match(result.subjectRef, /^[a-f0-9]{64}$/);
  assert.deepEqual(seen, []);
});
