/**
 * V0.1.0 UI-to-auth integration boundary. This file is not an authentication
 * implementation and is deliberately not wired into the Worker or preview.
 * Passwords go to the managed provider SDK; the Ryhze API accepts neither a
 * password nor browser-supplied identity/membership/verification claims.
 *
 * The live server implementation remains hosting/tester-worker.mjs, backed by
 * clerk-identity.mjs, csrf.mjs and d1-store.mjs. Reuse those audited boundaries
 * when replacing the disabled adapter; do not add a second token authority.
 */
export const accountContractVersion = 'invited-account-v1';

export const accountOperations = Object.freeze({
  signIn: Object.freeze({ authority: 'managed-provider', required: ['email', 'password'] }),
  signUp: Object.freeze({
    authority: 'managed-provider',
    required: ['providerInvitation', 'email', 'password'],
    transientClientOnly: ['passwordConfirmation'],
  }),
  verifyEmail: Object.freeze({ authority: 'managed-provider', required: ['verificationCode'] }),
  requestPasswordReset: Object.freeze({ authority: 'managed-provider', required: ['email'] }),
  resetPassword: Object.freeze({
    authority: 'managed-provider',
    required: ['resetCode', 'newPassword'],
    transientClientOnly: ['passwordConfirmation'],
  }),
  google: Object.freeze({
    authority: 'reviewed-oidc-provider',
    requiredScopes: Object.freeze(['openid', 'email']),
    forbiddenImports: Object.freeze(['name', 'given_name', 'family_name', 'picture']),
    activation: 'blocked-until-provider-request-and-storage-are-verified',
  }),
  session: Object.freeze({ authority: 'ryhze-server', method: 'GET', path: '/__auth/session' }),
  redeem: Object.freeze({ authority: 'ryhze-server', method: 'POST', path: '/__auth/redeem' }),
  profile: Object.freeze({ authority: 'ryhze-server', method: 'POST', path: '/__auth/profile' }),
  contentStatus: Object.freeze({
    authority: 'ryhze-server',
    method: 'GET',
    path: '/__auth/content-status',
  }),
});

/**
 * @typedef {Object} AccountServerAdapter
 * @property {() => Promise<Response>} session Authenticated server lookup only.
 * @property {() => Promise<Response>} redeem Provider-verified invitation only.
 * @property {(profile: {nickname: string, userId: string}) => Promise<Response>} profile
 *   Server canonicalization and atomic database uniqueness are required.
 * @property {() => Promise<Response>} contentStatus Independent membership/legal gate.
 */

const unavailable = async () =>
  Response.json(
    { code: 'provider_not_configured', available: false },
    {
      status: 503,
      headers: {
        'Cache-Control': 'private, no-store',
        'Referrer-Policy': 'no-referrer',
        'X-Content-Type-Options': 'nosniff',
      },
    },
  );

/** Explicit disabled skeleton, never a mock-success path or activation flag.
 * It ignores arguments without reading them, performs no request, writes no
 * storage, grants no session and returns no identity/account-enumeration data.
 * @returns {Readonly<AccountServerAdapter>}
 */
export function createDisabledAccountApi() {
  return Object.freeze({
    session: unavailable,
    redeem: unavailable,
    profile: unavailable,
    contentStatus: unavailable,
  });
}
