import test from 'node:test';
import assert from 'node:assert/strict';
import {
  canonicalUserId,
  canonicalNickname,
  canonicalInvitationEmail,
  invitationMatchesVerifiedEmail,
  canContinueTesterOnboarding,
} from '../auth/profile-policy.mjs';

test('user ID folds ASCII uppercase, preserves digits, dots and underscores', () => {
  assert.equal(canonicalUserId('Ryhze.User_123'), 'ryhze.user_123');
  for (const value of ['0', '007', 'a', 'a'.repeat(64), '.user_', 'a..b', '_'])
    assert.equal(canonicalUserId(value), value);
});

test('user ID rejects unsupported characters instead of silently rewriting them', () => {
  for (const value of [
    '',
    'a'.repeat(65),
    ' a',
    'a ',
    'a-b',
    'a/b',
    'ab\n',
    'é',
    'Ａ',
    'K',
    1,
    null,
  ])
    assert.equal(canonicalUserId(value), null, JSON.stringify(value));
});

test('email binding requires verified exact canonical email, without alias guesses', () => {
  assert.equal(canonicalInvitationEmail(' Tester+one@EXAMPLE.test '), 'tester+one@example.test');
  for (const value of ['', 'a@@b', 'a b@example.test', '\n', null])
    assert.equal(canonicalInvitationEmail(value), null);
  assert.equal(
    invitationMatchesVerifiedEmail('Person.Name@example.test', {
      email: 'person.name@EXAMPLE.TEST',
      emailVerified: true,
    }),
    true,
  );
  for (const identity of [
    { email: 'person.name@example.test', emailVerified: false },
    { email: 'person.name@example.test', emailVerified: 'true' },
    { email: 'personname@example.test', emailVerified: true },
    { email: 'person.name+tag@example.test', emailVerified: true },
    { email: 'other@example.test', emailVerified: true },
    undefined,
  ])
    assert.equal(invitationMatchesVerifiedEmail('Person.Name@example.test', identity), false);
});

test('onboarding policy is bound to provider, subject, email, active membership and expiry', () => {
  const input = {
    identity: {
      provider: 'synthetic',
      subject: 'person-1',
      email: 'tester@example.test',
      emailVerified: true,
    },
    membership: {
      provider: 'synthetic',
      subject: 'person-1',
      invitedEmail: 'tester@example.test',
      status: 'active',
      expiresAt: 2000,
    },
    now: 1000,
  };
  assert.equal(canContinueTesterOnboarding(input), true);
  assert.equal(canContinueTesterOnboarding(), false);
  for (const update of [
    { status: 'revoked' },
    { status: 'pending' },
    { expiresAt: 1000 },
    { expiresAt: '2000' },
    { subject: 'person-2' },
    { provider: 'other' },
    { invitedEmail: 'other@example.test' },
  ])
    assert.equal(
      canContinueTesterOnboarding({ ...input, membership: { ...input.membership, ...update } }),
      false,
    );
  assert.equal(
    canContinueTesterOnboarding({
      ...input,
      identity: { ...input.identity, emailVerified: false },
    }),
    false,
  );
  assert.equal(canContinueTesterOnboarding({ ...input, now: NaN }), false);
});

test('nickname preserves arbitrary Unicode, case, spaces and symbols with only technical limits', () => {
  for (const name of ['A', 'My Nick Name!', '星空✨', '👩🏽‍💻', '<b>Nick</b>', '😃'.repeat(80)])
    assert.equal(canonicalNickname(`  ${name}  `), name);
  for (const name of ['', '   ', 'a\u0000b', 'a\nb', 'a\tb', '\ud800', '😃'.repeat(81), null])
    assert.equal(canonicalNickname(name), null);
});
