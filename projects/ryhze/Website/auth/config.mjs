// Authentication preparation is deliberately distinct from legal admission.
// Never turn on an external hostname by changing this file alone.
export const release = '0.0.9';
export const defaultAuthConfig = Object.freeze({ mode: 'disabled' });
export const CLERK_JS_VERSION = '6.36.0';

export function validateAuthConfig(config) {
  if (config?.mode !== 'clerk-production') throw new Error('Tester auth is disabled');
  const origin = new URL(config.origin);
  const issuer = new URL(config.issuer);
  for (const url of [origin, issuer]) {
    if (
      url.protocol !== 'https:' ||
      url.origin !== url.href.slice(0, -1) ||
      url.port ||
      url.username ||
      url.password
    )
      throw new Error('Pinned HTTPS origins are required');
  }
  if (
    origin.origin !== config.origin ||
    issuer.origin !== config.issuer ||
    /(?:^|\.)clerk\.accounts\.dev$/.test(issuer.hostname)
  )
    throw new Error('Production Clerk instance required');
  if (
    !/^pk_live_[A-Za-z0-9+/=_-]+$/.test(config.publishableKey) ||
    !/^sk_live_[A-Za-z0-9]+$/.test(config.secretKey)
  )
    throw new Error('Production instance keys required');
  const frontend = atob(config.publishableKey.slice(8).replaceAll('-', '+').replaceAll('_', '/'));
  if (frontend !== issuer.hostname + '$') throw new Error('Publishable key and issuer mismatch');
  if (!/^-----BEGIN PUBLIC KEY-----\n[\s\S]+\n-----END PUBLIC KEY-----\s*$/.test(config.jwtKey))
    throw new Error('Pinned Clerk public key required');
  if (config.profilePolicy !== 'unicode-nickname-ascii-id-v1')
    throw new Error('Profile policy approval required');
  for (const key of ['provider', 'privacy', 'hosting', 'profile']) {
    if (typeof config.approvals?.[key] !== 'string' || !config.approvals[key].trim())
      throw new Error('Setup approval reference required');
  }
  if (
    config.googleEmailOnlyVerified === true &&
    (typeof config.googleScopeApproval !== 'string' || !config.googleScopeApproval.trim())
  )
    throw new Error('Verified email-only Google scope review is required');
  return config;
}

export function readAuthConfig(env = {}) {
  if (!env.RYHZE_TESTER_AUTH_MODE || env.RYHZE_TESTER_AUTH_MODE === 'disabled')
    return defaultAuthConfig;
  return validateAuthConfig({
    mode: env.RYHZE_TESTER_AUTH_MODE,
    origin: env.RYHZE_TESTER_ORIGIN,
    issuer: env.CLERK_ISSUER,
    publishableKey: env.CLERK_PUBLISHABLE_KEY,
    secretKey: env.CLERK_SECRET_KEY,
    jwtKey: env.CLERK_JWT_KEY,
    profilePolicy: env.RYHZE_PROFILE_POLICY,
    googleEmailOnlyVerified: env.RYHZE_GOOGLE_SCOPE_POLICY === 'openid-email-only-verified',
    googleScopeApproval: env.RYHZE_GOOGLE_SCOPE_APPROVAL,
    approvals: {
      provider: env.RYHZE_PROVIDER_APPROVAL,
      privacy: env.RYHZE_AUTH_PRIVACY_APPROVAL,
      hosting: env.RYHZE_TESTER_HOSTING_APPROVAL,
      profile: env.RYHZE_PROFILE_APPROVAL,
    },
  });
}
