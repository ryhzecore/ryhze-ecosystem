import { canonicalInvitationEmail } from './profile-policy.mjs';
// Scope consent must be verified BEFORE enabling the Google connection. This
// response-time check is defense in depth, not a substitute for minimizing the
// original OAuth request. The built-in provider's profile scope is NOT approved.
const EMAIL_ONLY = new Set(['openid', 'email', 'https://www.googleapis.com/auth/userinfo.email']);
export function acceptsGoogleIdentity(connections, verifiedEmail, emailOnlyVerified) {
  if (!Array.isArray(connections)) return false;
  if (connections.length === 0) return true; // Email/password-only Clerk identity.
  if (emailOnlyVerified !== true) return false;
  return connections.every((connection) => {
    if (
      canonicalInvitationEmail(connection.emailAddress) !==
        canonicalInvitationEmail(verifiedEmail) ||
      typeof connection.approvedScopes !== 'string'
    )
      return false;
    const scopes = connection.approvedScopes.trim().split(/\s+/);
    return (
      scopes.includes('openid') &&
      (scopes.includes('email') ||
        scopes.includes('https://www.googleapis.com/auth/userinfo.email')) &&
      scopes.every((scope) => EMAIL_ONLY.has(scope))
    );
  });
}
