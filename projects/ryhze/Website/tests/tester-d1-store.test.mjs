import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import { createTesterStore } from '../auth/d1-store.mjs';

// Actual local Workerd/D1 via the project's locked Wrangler dependency.
// No remote binding, network access, credential, live provider, or real invite.
const require = createRequire(import.meta.url);
const wranglerRequire = createRequire(require.resolve('wrangler/package.json'));
const { Miniflare } = await import(pathToFileURL(wranglerRequire.resolve('miniflare')).href);
const schema = await readFile(
  new URL('../auth/migrations/0001_tester_accounts.sql', import.meta.url),
  'utf8',
);
const identity = (number = 1) => ({
  provider: 'clerk',
  subject: `user_synthetic${number}`,
  sessionId: `sess_synthetic${number}`,
  email: `tester${number}@example.test`,
  emailVerified: true,
});

async function fixture(t) {
  const mf = new Miniflare({
    cf: false,
    telemetry: { enabled: false },
    workers: [
      {
        config: {
          name: 'local-tester-d1',
          compatibilityDate: '2026-10-01',
          manifest: {
            mainModule: 'worker.mjs',
            modules: {
              'worker.mjs': {
                type: 'esm',
                contents: 'export default {fetch(){return new Response("local only");}}',
              },
            },
          },
          env: { DB: { type: 'd1', id: 'local-only-tester-db' } },
        },
      },
    ],
  });
  t.after(() => mf.dispose());
  const db = await mf.getD1Database('DB');
  const statements = schema
    .replace(/--[^\n]*/g, '')
    .split(';')
    .map((s) => s.trim())
    .filter((s) => s && !s.startsWith('PRAGMA'));
  await db.batch(statements.map((s) => db.prepare(s)));
  const store = createTesterStore(db);
  const now = (await db.prepare('SELECT unixepoch()*1000 AS now').first()).now;
  async function invite(number = 1, overrides = {}) {
    const record = {
      id: `inv_synthetic${number}`,
      emailAddress: identity(number).email,
      status: 'accepted',
    };
    await db
      .prepare(
        `INSERT INTO tester_invitation
      (id,provider_invitation_id,email_canonical,created_at,redeem_before,access_expires_at,status)
      VALUES(?,?,?,?,?,?,?)`,
      )
      .bind(
        `invitation-${number}`,
        record.id,
        record.emailAddress,
        overrides.createdAt ?? now - 10000,
        overrides.redeemBefore ?? now + 60000,
        overrides.accessExpiresAt ?? now + 120000,
        overrides.status ?? 'pending',
      )
      .run();
    return record;
  }
  async function enroll(number = 1) {
    const record = await invite(number);
    assert.deepEqual(await store.redeemProviderInvitation(identity(number), record), {
      status: 'accepted',
    });
    return record;
  }
  return { db, store, now, invite, enroll };
}

test('actual D1: accepted Clerk invitation binds one verified identity, preserving separate expiry', async (t) => {
  const { store, invite, now } = await fixture(t);
  const record = await invite();
  assert.deepEqual(await store.resolvePendingInvitation(identity()), {
    providerInvitationId: record.id,
    emailAddress: record.emailAddress,
    redeemBefore: now + 60000,
    accessExpiresAt: now + 120000,
  });
  assert.equal(await store.resolveMembership(identity()), null);
  assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
    status: 'accepted',
  });
  const member = await store.resolveMembership(identity());
  assert.match(member.principalId, /^[0-9a-f]{8}-[0-9a-f-]{27}$/);
  assert.deepEqual(
    { ...member, principalId: null },
    { principalId: null, status: 'active', expiresAt: now + 120000, userId: null, nickname: null },
  );
  assert.equal(await store.resolvePendingInvitation(identity()), null);
  // Idempotent recovery after a response is lost, never a new membership.
  assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
    status: 'accepted',
  });
});

test('actual D1: wrong, unverified, pending, expired, or revoked invitation records never grant access', async (t) => {
  const { db, store, invite, now } = await fixture(t);
  const record = await invite();
  for (const principal of [
    { ...identity(), emailVerified: false },
    { ...identity(), emailVerified: 'true' },
    { ...identity(), provider: 'other' },
    { ...identity(), sessionId: '' },
    { ...identity(), email: 'other@example.test' },
    { ...identity(), email: 'tester1+alias@example.test' },
  ]) {
    assert.deepEqual(await store.redeemProviderInvitation(principal, record), {
      status: 'rejected',
    });
    assert.equal(await store.resolveMembership(principal), null);
  }
  for (const update of [
    { id: 'inv_other' },
    { emailAddress: 'other@example.test' },
    { status: 'pending' },
    { status: 'revoked' },
  ])
    assert.deepEqual(await store.redeemProviderInvitation(identity(), { ...record, ...update }), {
      status: 'rejected',
    });
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_principal').first()).n, 0);
  await db
    .prepare('UPDATE tester_invitation SET redeem_before=? WHERE id=?')
    .bind(now - 1, 'invitation-1')
    .run();
  assert.equal(await store.resolvePendingInvitation(identity()), null);
  assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
    status: 'rejected',
  });
  await db
    .prepare("UPDATE tester_invitation SET redeem_before=?,status='revoked' WHERE id=?")
    .bind(now + 60000, 'invitation-1')
    .run();
  assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
    status: 'rejected',
  });
});

test('actual D1: concurrent redemption cannot create two principals or grant a different subject', async (t) => {
  const { db, store, invite } = await fixture(t);
  const record = await invite();
  const results = await Promise.all([
    store.redeemProviderInvitation(identity(), record),
    store.redeemProviderInvitation({ ...identity(), subject: 'user_other' }, record),
    store.redeemProviderInvitation(identity(), record),
  ]);
  assert.ok(results.some((r) => r.status === 'accepted'));
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_principal').first()).n, 1);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_membership').first()).n, 1);
  const principal = await db.prepare('SELECT provider_subject FROM tester_principal').first();
  const losingIdentity = {
    ...identity(),
    subject: principal.provider_subject === 'user_other' ? identity().subject : 'user_other',
  };
  assert.equal(await store.resolveMembership(losingIdentity), null);
  assert.deepEqual(await store.redeemProviderInvitation(losingIdentity, record), {
    status: 'rejected',
  });
});

test('actual D1: a failed batch rolls back principal creation and invite consumption', async (t) => {
  const { db, invite } = await fixture(t);
  const record = await invite();
  const faultyStore = createTesterStore({
    prepare: db.prepare.bind(db),
    batch(statements) {
      return db.batch([
        statements[0],
        db.prepare('INSERT INTO nonexistent_table VALUES (1)'),
        ...statements.slice(1),
      ]);
    },
  });
  await assert.rejects(faultyStore.redeemProviderInvitation(identity(), record));
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_principal').first()).n, 0);
  assert.equal(
    (await db.prepare('SELECT status FROM tester_invitation').first()).status,
    'pending',
  );
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_membership').first()).n, 0);
});

test('actual D1: profile normalization, flexible valid characters and unique canonical claims', async (t) => {
  const { db, store, enroll } = await fixture(t);
  await enroll(1);
  await enroll(2);
  assert.deepEqual(
    await store.saveProfile(identity(), { userId: 'One.User_9', nickname: 'Nick_9.' }),
    { status: 'saved' },
  );
  let member = await store.resolveMembership(identity());
  assert.equal(member.userId, 'one.user_9');
  assert.equal(member.nickname, 'Nick_9.');
  assert.deepEqual(
    await store.saveProfile(identity(2), { userId: 'ONE.USER_9', nickname: 'another' }),
    { status: 'conflict' },
  );
  assert.deepEqual(await store.saveProfile(identity(2), { userId: 'a', nickname: '007' }), {
    status: 'saved',
  });
  assert.deepEqual(await store.saveProfile(identity(2), { userId: '007', nickname: 'a.._' }), {
    status: 'saved',
  });
  for (const invalid of ['', 'a b', 'a-b', 'a/b', 'é', 'K', 'Ａ', 'a\n', 'x'.repeat(65)])
    assert.deepEqual(await store.saveProfile(identity(), { userId: invalid, nickname: 'valid' }), {
      status: 'rejected',
    });
  for (const reserved of ['Admin', 'SUPPORT', 'Ryhze'])
    assert.deepEqual(await store.saveProfile(identity(), { userId: reserved, nickname: 'valid' }), {
      status: 'rejected',
    });
  assert.deepEqual(
    await store.saveProfile(identity(), { userId: 'valid', nickname: 'Contains\nControl' }),
    { status: 'rejected' },
  );
  const freeNickname = "A'); DROP TABLE tester_profile; -- <script> 😃";
  assert.deepEqual(
    await store.saveProfile(identity(), { userId: 'valid', nickname: `  ${freeNickname}  ` }),
    { status: 'saved' },
  );
  assert.equal((await store.resolveMembership(identity())).nickname, freeNickname);
  assert.deepEqual(
    await store.saveProfile(identity(), { userId: 'valid', nickname: '😃'.repeat(80) }),
    { status: 'saved' },
  );
  assert.deepEqual(
    await store.saveProfile(identity(), { userId: 'valid', nickname: '😃'.repeat(81) }),
    { status: 'rejected' },
  );
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_profile').first()).n, 2);
});

test('actual D1: parallel profile claims have exactly one winner', async (t) => {
  const { db, store, enroll } = await fixture(t);
  await enroll(1);
  await enroll(2);
  const results = await Promise.all([
    store.saveProfile(identity(1), { userId: 'CLAIMED_1', nickname: 'one' }),
    store.saveProfile(identity(2), { userId: 'claimed_1', nickname: 'two' }),
  ]);
  assert.deepEqual(results.map((x) => x.status).sort(), ['conflict', 'saved']);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_profile').first()).n, 1);
});

test('actual D1: revocation, suspension, membership expiry and email drift deny reads and writes', async (t) => {
  const { db, store, enroll, now } = await fixture(t);
  const record = await enroll();
  const input = { userId: 'valid', nickname: 'valid' };
  assert.equal(
    await store.resolveMembership({ ...identity(), email: 'changed@example.test' }),
    null,
  );
  assert.deepEqual(
    await store.saveProfile({ ...identity(), email: 'changed@example.test' }, input),
    { status: 'rejected' },
  );
  for (const [revoke, restore] of [
    [
      "UPDATE tester_principal SET status='suspended'",
      "UPDATE tester_principal SET status='active'",
    ],
    ["UPDATE tester_principal SET status='revoked'", "UPDATE tester_principal SET status='active'"],
    [
      "UPDATE tester_membership SET status='suspended'",
      "UPDATE tester_membership SET status='active'",
    ],
    [
      "UPDATE tester_membership SET status='revoked',revoked_at=unixepoch()*1000",
      "UPDATE tester_membership SET status='active',revoked_at=NULL",
    ],
    [
      "UPDATE tester_invitation SET status='revoked'",
      "UPDATE tester_invitation SET status='redeemed'",
    ],
  ]) {
    await db.prepare(revoke).run();
    assert.equal(await store.resolveMembership(identity()), null);
    assert.deepEqual(await store.saveProfile(identity(), input), { status: 'rejected' });
    assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
      status: 'rejected',
    });
    await db.prepare(restore).run();
  }
  // Invitation link redemption expiry does not end an already granted membership.
  await db
    .prepare('UPDATE tester_invitation SET redeem_before=?')
    .bind(now - 1)
    .run();
  assert.ok(await store.resolveMembership(identity()));
  await db
    .prepare('UPDATE tester_membership SET created_at=?,access_expires_at=?')
    .bind(now - 10000, now - 1)
    .run();
  assert.equal(await store.resolveMembership(identity()), null);
  assert.deepEqual(await store.saveProfile(identity(), input), { status: 'rejected' });
  assert.deepEqual(await store.redeemProviderInvitation(identity(), record), {
    status: 'rejected',
  });
});

test('actual D1: outsider session and SQL-like input cannot create membership or bypass checks', async (t) => {
  const { db, store, invite } = await fixture(t);
  await invite();
  assert.equal(await store.resolveMembership(identity()), null);
  assert.deepEqual(await store.saveProfile(identity(), { userId: 'abc', nickname: 'abc' }), {
    status: 'rejected',
  });
  assert.equal(await store.resolveMembership({ ...identity(), subject: "x'OR'1'='1" }), null);
  assert.equal(
    await store.resolvePendingInvitation({ ...identity(), email: "x'OR'1'='1@example.test" }),
    null,
  );
  assert.equal((await db.prepare('SELECT count(*) AS n FROM tester_principal').first()).n, 0);
});

test('profile policy stays configurable and invalid configuration fails closed', () => {
  const db = {
    prepare() {
      throw new Error('D1 unavailable');
    },
    batch() {
      throw new Error('D1 unavailable');
    },
  };
  assert.throws(() => createTesterStore(undefined), /D1 binding/);
  for (const profilePolicy of [
    { minLength: 0 },
    { maxLength: 65 },
    { minLength: 10, maxLength: 5 },
    { reservedUserIds: ['Bad Space'] },
  ])
    assert.throws(() => createTesterStore(db, { profilePolicy }), /Invalid/);
  assert.ok(
    createTesterStore(db, { profilePolicy: { minLength: 1, maxLength: 64, reservedUserIds: [] } }),
  );
});

test('D1 operational failures propagate instead of becoming a successful or conflicting form result', async () => {
  const unavailable = new Error('D1 unavailable');
  const store = createTesterStore({
    prepare() {
      throw unavailable;
    },
    batch() {
      throw unavailable;
    },
  });
  await assert.rejects(store.resolveMembership(identity()), (e) => e === unavailable);
  await assert.rejects(store.resolvePendingInvitation(identity()), (e) => e === unavailable);
  await assert.rejects(
    store.saveProfile(identity(), { userId: 'abc', nickname: 'abc' }),
    (e) => e === unavailable,
  );
});
