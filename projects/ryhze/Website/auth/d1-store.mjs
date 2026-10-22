import { canonicalInvitationEmail, canonicalUserId, canonicalNickname } from './profile-policy.mjs';

// Provisional product defaults; the alphabet and canonical uniqueness are also
// enforced in SQL. The owner may change these bounds before real enrollment.
export const DEFAULT_PROFILE_POLICY = Object.freeze({
  minLength: 1,
  maxLength: 64,
  reservedUserIds: Object.freeze(['admin', 'support', 'ryhze']),
});

const ACTIVE_MEMBERSHIP = `
  FROM tester_principal p
  JOIN tester_membership m ON m.principal_id = p.id
  JOIN tester_invitation i ON i.id = m.invitation_id AND i.redeemed_by = p.id
  WHERE p.provider = ? AND p.provider_subject = ? AND p.email_canonical = ?
    AND p.status = 'active' AND m.status = 'active'
    AND m.access_expires_at > unixepoch() * 1000
    AND i.status = 'redeemed' AND i.email_canonical = p.email_canonical`;

function opaqueId(value) {
  return (
    typeof value === 'string' &&
    value.length > 0 &&
    value.length <= 255 &&
    !/[\s\x00-\x1f\x7f]/.test(value)
  );
}

/** Accepts only a SERVER-verified Clerk identity, never body/query claims. */
function identityValues(identity) {
  if (
    identity?.provider !== 'clerk' ||
    !opaqueId(identity.subject) ||
    !opaqueId(identity.sessionId) ||
    identity.emailVerified !== true
  )
    return null;
  const email = canonicalInvitationEmail(identity.email);
  return email ? ['clerk', identity.subject, email] : null;
}

function readProfilePolicy(policy) {
  const resolved = { ...DEFAULT_PROFILE_POLICY, ...policy };
  if (
    !Number.isInteger(resolved.minLength) ||
    !Number.isInteger(resolved.maxLength) ||
    resolved.minLength < 1 ||
    resolved.maxLength > 64 ||
    resolved.minLength > resolved.maxLength ||
    !Array.isArray(resolved.reservedUserIds)
  )
    throw new TypeError('Invalid tester profile policy.');
  const reserved = resolved.reservedUserIds.map(canonicalUserId);
  if (reserved.some((value) => value === null)) throw new TypeError('Invalid reserved user ID.');
  return { ...resolved, reservedUserIds: new Set(reserved) };
}

/**
 * Cloudflare D1 application authorization store. Methods never create Clerk
 * users, invitations or secrets. Provider records must come from authenticated
 * Clerk Backend API reads whose instance is pinned by the calling Worker.
 *
 * All timestamps are epoch milliseconds; authorization time is database-owned.
 * Operational errors are intentionally propagated so the Worker can fail closed.
 */
export function createTesterStore(db, { profilePolicy } = {}) {
  if (typeof db?.prepare !== 'function' || typeof db?.batch !== 'function')
    throw new TypeError('A D1 binding is required.');
  const policy = readProfilePolicy(profilePolicy);
  const statement = (sql, values) => db.prepare(sql).bind(...values);

  async function resolveMembership(identity) {
    const values = identityValues(identity);
    if (!values) return null;
    const row = await statement(
      `
      SELECT p.id AS principalId, m.status, m.access_expires_at AS expiresAt,
        (SELECT public_user_id FROM tester_profile WHERE principal_id = p.id) AS userId,
        (SELECT nickname FROM tester_profile WHERE principal_id = p.id) AS nickname
      ${ACTIVE_MEMBERSHIP}`,
      values,
    ).first();
    return row ?? null;
  }

  async function resolvePendingInvitation(identity) {
    const values = identityValues(identity);
    if (!values) return null;
    return (
      (await statement(
        `
      SELECT provider_invitation_id AS providerInvitationId,
        email_canonical AS emailAddress, redeem_before AS redeemBefore,
        access_expires_at AS accessExpiresAt
      FROM tester_invitation
      WHERE email_canonical = ? AND status = 'pending'
        AND redeem_before > unixepoch() * 1000
        AND access_expires_at > unixepoch() * 1000`,
        [values[2]],
      ).first()) ?? null
    );
  }

  async function redeemProviderInvitation(identity, trustedRecord) {
    const values = identityValues(identity);
    if (
      !values ||
      !opaqueId(trustedRecord?.id) ||
      trustedRecord.status !== 'accepted' ||
      canonicalInvitationEmail(trustedRecord.emailAddress) !== values[2]
    )
      return { status: 'rejected' };
    const providerInvitationId = trustedRecord.id;
    const candidateId = crypto.randomUUID();
    // Each request's four statements form one D1 batch transaction. Every
    // mutation repeats the authoritative conditions; no pre-read authorizes it.
    // An interrupted successful response can be retried by the SAME verified
    // principal, while another subject cannot consume the existing membership.
    const results = await db.batch([
      statement(
        `
        INSERT INTO tester_principal (id, provider, provider_subject, email_canonical)
        SELECT ?, ?, ?, ?
        WHERE EXISTS (
          SELECT 1 FROM tester_invitation
          WHERE provider_invitation_id = ? AND email_canonical = ?
            AND status = 'pending' AND redeem_before > unixepoch() * 1000
            AND access_expires_at > unixepoch() * 1000
        ) AND NOT EXISTS (
          SELECT 1 FROM tester_principal WHERE email_canonical = ?
            AND (provider <> ? OR provider_subject <> ?)
        )
        ON CONFLICT(provider, provider_subject) DO NOTHING`,
        [candidateId, ...values, providerInvitationId, values[2], values[2], values[0], values[1]],
      ),
      statement(
        `
        UPDATE tester_invitation SET status = 'redeemed', redeemed_at = unixepoch() * 1000,
          redeemed_by = (
            SELECT id FROM tester_principal WHERE provider = ? AND provider_subject = ?
              AND email_canonical = ? AND status = 'active'
          )
        WHERE provider_invitation_id = ? AND email_canonical = ? AND status = 'pending'
          AND redeem_before > unixepoch() * 1000 AND access_expires_at > unixepoch() * 1000
          AND EXISTS (
            SELECT 1 FROM tester_principal p WHERE p.provider = ? AND p.provider_subject = ?
              AND p.email_canonical = ? AND p.status = 'active'
              AND NOT EXISTS (SELECT 1 FROM tester_membership WHERE principal_id = p.id)
          )`,
        [...values, providerInvitationId, values[2], ...values],
      ),
      statement(
        `
        INSERT INTO tester_membership (principal_id, invitation_id, access_expires_at)
        SELECT p.id, i.id, i.access_expires_at
        FROM tester_invitation i JOIN tester_principal p ON p.id = i.redeemed_by
        WHERE i.provider_invitation_id = ? AND i.status = 'redeemed'
          AND i.email_canonical = ? AND i.access_expires_at > unixepoch() * 1000
          AND p.provider = ? AND p.provider_subject = ? AND p.email_canonical = ? AND p.status = 'active'
        ON CONFLICT(principal_id) DO NOTHING`,
        [providerInvitationId, values[2], ...values],
      ),
      statement(`SELECT p.id ${ACTIVE_MEMBERSHIP} AND i.provider_invitation_id = ?`, [
        ...values,
        providerInvitationId,
      ]),
    ]);
    return { status: results[3]?.results?.length === 1 ? 'accepted' : 'rejected' };
  }

  async function saveProfile(identity, input) {
    const values = identityValues(identity);
    if (!values) return { status: 'rejected' };
    const userId = canonicalUserId(input?.userId);
    const nickname = canonicalNickname(input?.nickname);
    if (
      !userId ||
      !nickname ||
      userId.length < policy.minLength ||
      userId.length > policy.maxLength ||
      policy.reservedUserIds.has(userId)
    )
      return { status: 'rejected' };
    try {
      const result = await statement(
        `
        INSERT INTO tester_profile (principal_id, public_user_id, nickname)
        SELECT p.id, ?, ? ${ACTIVE_MEMBERSHIP}
        ON CONFLICT(principal_id) DO UPDATE SET
          public_user_id = excluded.public_user_id, nickname = excluded.nickname,
          updated_at = unixepoch() * 1000`,
        [userId, nickname, ...values],
      ).run();
      return { status: result.meta?.changes === 1 ? 'saved' : 'rejected' };
    } catch (error) {
      // Only this expected constraint is a public handle conflict. Do not turn
      // a missing binding/schema/database outage into a misleading form error.
      if (
        /UNIQUE constraint failed: tester_profile\.public_user_id(?:\b|$)/.test(
          String(error?.message),
        )
      )
        return { status: 'conflict' };
      throw error;
    }
  }

  return Object.freeze({
    resolveMembership,
    resolvePendingInvitation,
    redeemProviderInvitation,
    saveProfile,
  });
}
