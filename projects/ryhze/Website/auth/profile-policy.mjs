/**
 * Provider-independent V0.0.9 profile contract. This does not authenticate a user,
 * create an account, issue an invitation, or enable the checked-in Worker.
 */
export const USER_ID_MAX_LENGTH = 64;

/**
 * Public user IDs are distinct from opaque auth subject IDs. ASCII letters are
 * case-folded only; no Unicode lookalike folding or silent character removal.
 * The generous length bound is a proposed local validation default.
 */
export function canonicalUserId(input) {
  if (typeof input !== 'string' || input.length === 0 || input.length > USER_ID_MAX_LENGTH)
    return null;
  if (!/^[A-Za-z0-9._]+$/.test(input)) return null;
  return input.toLowerCase();
}

/** Nicknames preserve the user's Unicode/case/symbols. Only technical bounds
 * apply: trim surrounding whitespace, reject empty/control/ill-formed text, and
 * cap at 80 Unicode code points. Always escape at the eventual rendering sink.
 */
export function canonicalNickname(input) {
  if (typeof input !== 'string' || !input.isWellFormed()) return null;
  const nickname = input.trim();
  if (
    nickname.length === 0 ||
    [...nickname].length > 80 ||
    /[\u0000-\u001f\u007f-\u009f]/.test(nickname)
  )
    return null;
  return nickname;
}

/**
 * Equality policy for invitation email binding. The auth provider must first
 * verify the address. Never drop dots or plus-address tags or infer an alias.
 * Full address syntax validation belongs to the selected auth provider.
 */
export function canonicalInvitationEmail(input) {
  if (typeof input !== 'string' || input.length > 254) return null;
  const email = input.trim();
  if (!/^[\x21-\x7e]+@[\x21-\x7e]+$/.test(email) || email.split('@').length !== 2) return null;
  return email.toLowerCase();
}

export function invitationMatchesVerifiedEmail(invitationEmail, identity) {
  if (identity?.emailVerified !== true) return false;
  const invited = canonicalInvitationEmail(invitationEmail);
  return invited !== null && invited === canonicalInvitationEmail(identity.email);
}

/** Pure authorization policy, not an identity verifier. All inputs are trusted
 * server-side database/provider results, never request-body or localStorage flags.
 * Legal approval remains independently enforced by the existing agreement gate.
 */
export function canContinueTesterOnboarding({ identity, membership, now } = {}) {
  return Boolean(
    typeof identity?.subject === 'string' &&
    identity.subject.length > 0 &&
    typeof identity?.provider === 'string' &&
    identity.provider.length > 0 &&
    Number.isSafeInteger(now) &&
    membership?.status === 'active' &&
    membership.provider === identity.provider &&
    membership.subject === identity.subject &&
    invitationMatchesVerifiedEmail(membership.invitedEmail, identity) &&
    Number.isSafeInteger(membership.expiresAt) &&
    membership.expiresAt > now,
  );
}
