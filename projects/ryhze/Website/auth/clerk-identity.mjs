import { verifyToken } from '@clerk/backend';
import { createClerkReadClient } from './clerk-read-api.mjs';
import { canonicalInvitationEmail } from './profile-policy.mjs';
import { validateAuthConfig } from './config.mjs';
import { acceptsGoogleIdentity } from './google-privacy.mjs';

function sessionToken(request) {
  const bearer = request.headers.get('authorization');
  if (bearer !== null) return /^Bearer [A-Za-z0-9_.-]+$/.test(bearer) ? bearer.slice(7) : null;
  const cookies = (request.headers.get('cookie') ?? '').split(';').map((v) => v.trim());
  const sessions = cookies.filter((v) => v.startsWith('__session='));
  if (sessions.length !== 1) return null;
  const token = sessions[0].slice(10);
  return /^[A-Za-z0-9_.-]+$/.test(token) ? token : null;
}

/** Official Clerk SDK verifies the signed JWT; server API confirms current
 * revocation status and verified primary email. The app never trusts frontend
 * metadata, Google profile data supplied by the browser, or JWT email fields. */
export function createClerkIdentity(config, { client } = {}) {
  validateAuthConfig(config);
  const clerk = client ?? createClerkReadClient(config.secretKey);
  return {
    async resolve(request) {
      const token = sessionToken(request);
      if (!token || token.length > 8192) return null;
      try {
        const claims = await verifyToken(token, {
          jwtKey: config.jwtKey,
          authorizedParties: [config.origin],
          clockSkewInMs: 0,
        });
        if (
          claims.iss !== config.issuer ||
          claims.azp !== config.origin ||
          typeof claims.sub !== 'string' ||
          !claims.sub.startsWith('user_') ||
          typeof claims.sid !== 'string' ||
          !claims.sid.startsWith('sess_') ||
          !Number.isSafeInteger(claims.exp) ||
          !Number.isSafeInteger(claims.iat) ||
          claims.iat * 1000 > Date.now() ||
          claims.exp <= claims.iat ||
          claims.exp - claims.iat > 120 ||
          claims.sts === 'pending' ||
          claims.act != null
        )
          return null;
        const session = await clerk.sessions.getSession(claims.sid);
        if (
          session.id !== claims.sid ||
          session.userId !== claims.sub ||
          session.status !== 'active' ||
          !Number.isSafeInteger(session.expireAt) ||
          session.expireAt <= Date.now()
        )
          return null;
        const user = await clerk.users.getUser(claims.sub);
        if (user.id !== claims.sub || user.banned || user.locked || user.unsupportedConnections > 0)
          return null;
        const primary = user.emailAddresses.find(
          (email) => email.id === user.primaryEmailAddressId,
        );
        const email = canonicalInvitationEmail(primary?.emailAddress);
        if (
          !email ||
          primary.verification?.status !== 'verified' ||
          claims.exp * 1000 <= Date.now() ||
          session.expireAt <= Date.now()
        )
          return null;
        if (
          !acceptsGoogleIdentity(
            user.googleConnections ?? [],
            email,
            config.googleEmailOnlyVerified,
          )
        )
          return null;
        return {
          provider: 'clerk',
          subject: user.id,
          sessionId: session.id,
          email,
          emailVerified: true,
        };
      } catch {
        return null;
      }
    },
    async acceptedInvitation(providerInvitationId) {
      // The SDK exposes paginated invitation listing; its query is server-only.
      // The exact ID is checked after the documented substring query.
      const list = await clerk.invitations.getInvitationList({
        query: providerInvitationId,
        status: 'accepted',
        limit: 100,
      });
      const matches = list.data.filter(
        (invite) =>
          invite.id === providerInvitationId &&
          invite.status === 'accepted' &&
          invite.revoked !== true,
      );
      return matches.length === 1
        ? { id: matches[0].id, emailAddress: matches[0].emailAddress, status: 'accepted' }
        : null;
    },
  };
}
