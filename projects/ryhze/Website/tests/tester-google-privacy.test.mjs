import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { acceptsGoogleIdentity } from '../auth/google-privacy.mjs';
const email = 'tester@example.test';
test('Google connection is disabled until actual email-only OAuth scope verification', () => {
  const connection = { emailAddress: email, approvedScopes: 'openid email' };
  assert.equal(acceptsGoogleIdentity([], email, false), true);
  assert.equal(acceptsGoogleIdentity([connection], email, false), false);
  assert.equal(acceptsGoogleIdentity([connection], email, true), true);
  assert.equal(
    acceptsGoogleIdentity(
      [{ ...connection, approvedScopes: 'openid https://www.googleapis.com/auth/userinfo.email' }],
      email,
      true,
    ),
    true,
  );
});
test('Google profile, photos, added Google APIs, wrong email and unknown scopes are denied', () => {
  for (const approvedScopes of [
    'openid email profile',
    'openid email https://www.googleapis.com/auth/userinfo.profile',
    'openid email https://www.googleapis.com/auth/drive',
    'email',
    'openid',
    '',
    null,
  ])
    assert.equal(
      acceptsGoogleIdentity([{ emailAddress: email, approvedScopes }], email, true),
      false,
    );
  assert.equal(
    acceptsGoogleIdentity(
      [{ emailAddress: 'other@example.test', approvedScopes: 'openid email' }],
      email,
      true,
    ),
    false,
  );
});
test('Ryhze does not request profile scope, retrieve OAuth tokens, or prefill nickname/photo from Google', async () => {
  const controller = await readFile(new URL('../auth/ui/controller.mjs', import.meta.url), 'utf8');
  const database = await readFile(
    new URL('../auth/migrations/0001_tester_accounts.sql', import.meta.url),
    'utf8',
  );
  const api = await readFile(new URL('../auth/clerk-read-api.mjs', import.meta.url), 'utf8');
  assert.doesNotMatch(
    controller,
    /additionalScopes|profileScope|clerk\.user|imageUrl|firstName|lastName|fullName|avatar/i,
  );
  assert.doesNotMatch(database, /avatar|image_url|first_name|last_name|password|oauth_token/i);
  assert.doesNotMatch(
    api,
    /getUserOauthAccessToken|oauth_access_tokens|first_name|last_name|image_url|avatar_url/,
  );
});
