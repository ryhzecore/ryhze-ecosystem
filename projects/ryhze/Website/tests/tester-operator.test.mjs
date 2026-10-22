import test from 'node:test';
import assert from 'node:assert/strict';
import { planTesterAction, parseArguments } from '../scripts/manage-testers.mjs';
const now = Date.parse('2026-10-01T00:00:00Z');
const input = {
  action: 'invite',
  email: 'Person+one@Example.test',
  requestRef: 'synthetic-owner-request',
  origin: 'https://testers.example.test',
  redeemBefore: '2026-10-08T00:00:00Z',
  accessExpiresAt: '2026-10-15T00:00:00Z',
};
test('operator helper is explicit, exact-email bound, parameterized and always dry-run', () => {
  const plan = planTesterAction(input, now);
  assert.equal(plan.dryRun, true);
  assert.equal(plan.clerk.params.notify, false);
  assert.equal(plan.clerk.params.ignoreExisting, false);
  assert.equal(plan.clerk.params.emailAddress, 'person+one@example.test');
  assert.equal(plan.clerk.params.expiresInDays, 7);
  assert.equal(plan.clerk.params.redirectUrl, input.origin + '/__auth/sign-up');
  assert.equal(plan.database.params.length, 5);
  assert.ok(!plan.database.sql.includes('example.test'));
});
test('operator refuses implicit expiry, missing request, malformed destination and apply options', () => {
  for (const update of [
    { requestRef: '' },
    { redeemBefore: null },
    { accessExpiresAt: undefined },
    { redeemBefore: '2026-09-30T00:00:00Z' },
    { origin: 'http://localhost:3000' },
    { origin: 'https://testers.example.test/path' },
    { email: 'not-an-email' },
  ])
    assert.throws(() => planTesterAction({ ...input, ...update }, now));
  assert.throws(() => parseArguments(['--apply', 'true']));
  assert.throws(() => parseArguments(['--email', 'x', '--email', 'y']));
});
test('suspension and revocation plans contain no external calls and preserve history', () => {
  for (const action of ['suspend', 'revoke']) {
    const plan = planTesterAction({ ...input, action }, now);
    assert.equal(plan.dryRun, true);
    assert.equal(plan.clerk, undefined);
    assert.ok(!JSON.stringify(plan).includes('DELETE'));
  }
});
