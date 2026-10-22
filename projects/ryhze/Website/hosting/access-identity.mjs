import { createRemoteJWKSet, jwtVerify } from 'jose';
import { createLoginMethodVerifier } from './access-login-method.mjs';
import { reportAccessFailure } from './access-denial-diagnostics.mjs';

const encoder = new TextEncoder();

async function digest(value) {
  const bytes = await crypto.subtle.digest('SHA-256', encoder.encode(value));
  return Array.from(new Uint8Array(bytes), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

/** Only a configured Cloudflare Access tenant is a trusted key issuer. */
export function validateAccessConfig({ issuer, audience, allowedEmails }) {
  const url = new URL(issuer);
  if (
    url.protocol !== 'https:' ||
    !/^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.cloudflareaccess\.com$/.test(url.hostname) ||
    url.port ||
    url.username ||
    url.password ||
    url.pathname !== '/' ||
    url.search ||
    url.hash ||
    issuer !== url.origin
  )
    throw new Error('A verified Cloudflare Access tenant origin is required');
  if (typeof audience !== 'string' || !audience.trim())
    throw new Error('Access audience is required');
  if (
    !Array.isArray(allowedEmails) ||
    allowedEmails.length === 0 ||
    allowedEmails.some(
      (email) => typeof email !== 'string' || !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email),
    )
  ) {
    throw new Error('Explicit approved email allowlist is required');
  }
  return { issuer, audience, allowedEmails: allowedEmails.map((email) => email.toLowerCase()) };
}

/**
 * Verify Access's signed assertion, never a plain email header or browser flag.
 * A supplied key resolver is for isolated tests; the deployed adapter uses the
 * pinned tenant's HTTPS JWKS endpoint. Raw identity and tokens are not logged.
 */
export function createAccessIdentity(
  input,
  {
    keyResolver,
    clock = () => new Date(),
    requireApplicationToken = false,
    loginMethods,
    identityFetch,
  } = {},
) {
  const config = validateAccessConfig(input);
  const keys =
    keyResolver ??
    createRemoteJWKSet(new URL(`${config.issuer}/cdn-cgi/access/certs`), {
      timeoutDuration: 5000,
      cooldownDuration: 30000,
      cacheMaxAge: 300000,
    });
  const verifyLoginMethod = loginMethods
    ? createLoginMethodVerifier({ issuer: config.issuer, methods: loginMethods }, { identityFetch })
    : null;
  return {
    async resolve(request, onFailure) {
      const deny = (code) => reportAccessFailure(onFailure, code, null);
      const token = request.headers.get('cf-access-jwt-assertion');
      if (!token) return deny('RZ-A01');
      if (token.length > 16384) return deny('RZ-A02');
      try {
        const { payload } = await jwtVerify(token, keys, {
          algorithms: ['RS256'],
          issuer: config.issuer,
          audience: config.audience,
          requiredClaims: ['iss', 'aud', 'sub', 'iat', 'exp'],
          currentDate: clock(),
          clockTolerance: 0,
        });
        if (
          (requireApplicationToken && payload.type !== 'app') ||
          typeof payload.sub !== 'string' ||
          !payload.sub ||
          typeof payload.email !== 'string' ||
          !config.allowedEmails.includes(payload.email.toLowerCase()) ||
          typeof payload.iat !== 'number' ||
          payload.iat > clock().getTime() / 1000 ||
          payload.exp <= clock().getTime() / 1000
        )
          return deny('RZ-A03');
        if (verifyLoginMethod && !(await verifyLoginMethod({ token, payload, onFailure })))
          return null;
        // Network verification must not extend the lifetime of the signed assertion.
        if (payload.exp <= clock().getTime() / 1000) return deny('RZ-A11');
        return {
          subjectRef: await digest(`${config.issuer}\u0000${payload.sub}`),
          sessionRef: await digest(token),
        };
      } catch {
        // Invalid, expired, unapproved, or unverifiable identities all deny.
        // Only a finite diagnostic enum may leave; never tokens or identifying claims.
        return deny('RZ-A03');
      }
    },
  };
}
