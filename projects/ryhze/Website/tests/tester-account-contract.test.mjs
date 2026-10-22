import assert from 'node:assert/strict';
import test from 'node:test';
import { accountOperations, createDisabledAccountApi } from '../auth/account-contract.mjs';

test('all disabled server adapter actions fail closed without consuming submitted data', async () => {
  const api = createDisabledAccountApi();
  assert.ok(Object.isFrozen(api));
  const forbidden = new Proxy(
    {},
    {
      get() {
        throw new Error('Must not inspect submitted data');
      },
    },
  );
  for (const action of Object.values(api)) {
    const response = await action(forbidden);
    assert.equal(response.status, 503);
    assert.equal(response.headers.get('Cache-Control'), 'private, no-store');
    assert.equal(response.headers.get('Referrer-Policy'), 'no-referrer');
    assert.equal(response.headers.get('Set-Cookie'), null);
    assert.deepEqual(await response.json(), { code: 'provider_not_configured', available: false });
  }
});

test('contract keeps credentials with provider and reuses existing Ryhze server paths', () => {
  for (const action of ['signIn', 'signUp', 'verifyEmail', 'requestPasswordReset', 'resetPassword'])
    assert.equal(accountOperations[action].authority, 'managed-provider');
  assert.deepEqual(accountOperations.signUp.transientClientOnly, ['passwordConfirmation']);
  assert.deepEqual(accountOperations.resetPassword.transientClientOnly, ['passwordConfirmation']);
  assert.deepEqual(
    ['session', 'redeem', 'profile', 'contentStatus'].map((key) => accountOperations[key].path),
    ['/__auth/session', '/__auth/redeem', '/__auth/profile', '/__auth/content-status'],
  );
  assert.equal(
    accountOperations.google.activation,
    'blocked-until-provider-request-and-storage-are-verified',
  );
  assert.deepEqual(accountOperations.google.requiredScopes, ['openid', 'email']);
  assert.ok(accountOperations.google.forbiddenImports.includes('picture'));
});
