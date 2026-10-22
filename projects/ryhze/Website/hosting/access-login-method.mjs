/**
 * The Access application JWT does not promise an IdP claim. Retrieve the full
 * identity from the pinned issuer only, after cryptographic JWT validation.
 * Never infer Google login from a Gmail address, AMR, a query or an email header.
 */
import { reportAccessFailure } from './access-denial-diagnostics.mjs';

const maxIdentityBytes = 16384;
const providerId = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/i;

export function validateLoginMethods(methods) {
  if (
    !Array.isArray(methods) ||
    methods.length < 1 ||
    methods.length > 2 ||
    methods.some(
      (method) =>
        !method ||
        typeof method.id !== 'string' ||
        !providerId.test(method.id) ||
        !['google', 'onetimepin'].includes(method.type),
    ) ||
    new Set(methods.map(({ id }) => id.toLowerCase())).size !== methods.length ||
    new Set(methods.map(({ type }) => type)).size !== methods.length
  )
    throw new Error('Explicit supported Access identity providers are required');
  // Snapshot: a later mutation cannot broaden the configured provider list.
  return methods.map(({ id, type }) => Object.freeze({ id, type }));
}

async function readIdentity(response, signal) {
  if (
    response.status !== 200 ||
    response.redirected ||
    !/^application\/json(?:\s*;|$)/i.test(response.headers.get('Content-Type') ?? '') ||
    !response.body
  )
    throw new Error('Identity unavailable');
  const declaredLength = response.headers.get('Content-Length');
  if (
    declaredLength &&
    (!/^\d+$/.test(declaredLength) || Number(declaredLength) > maxIdentityBytes)
  )
    throw new Error('Identity unavailable');
  const reader = response.body.getReader();
  const cancel = () => {
    void reader.cancel().catch(() => {});
  };
  signal.addEventListener('abort', cancel, { once: true });
  let total = 0;
  const chunks = [];
  try {
    while (true) {
      if (signal.aborted) throw new Error('Identity unavailable');
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > maxIdentityBytes) throw new Error('Identity unavailable');
      chunks.push(value);
    }
  } finally {
    signal.removeEventListener('abort', cancel);
    cancel();
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
}

export function createLoginMethodVerifier({ issuer, methods }, { identityFetch = fetch } = {}) {
  const approved = validateLoginMethods(methods);
  const endpoint = `${issuer}/cdn-cgi/access/get-identity`;
  return async function verify({ token, payload, onFailure }) {
    const deny = (code) => reportAccessFailure(onFailure, code, false);
    if (
      payload.type !== 'app' ||
      typeof payload.identity_nonce !== 'string' ||
      !payload.identity_nonce ||
      payload.identity_nonce.length > 256
    )
      return deny('RZ-A04');
    const controller = new AbortController();
    const deadline = performance.now() + 5000;
    let timeout;
    const expired = new Promise((resolve) => {
      timeout = setTimeout(() => {
        controller.abort();
        resolve(deny('RZ-A06'));
      }, 5000);
    });
    let failureCode = 'RZ-A05';
    const lookup = async () => {
      const response = await identityFetch(endpoint, {
        method: 'GET',
        headers: { Accept: 'application/json', Cookie: `CF_Authorization=${token}` },
        redirect: 'manual',
        cache: 'no-store',
        signal: controller.signal,
      });
      // A supplied test transport does not create a production identity bypass.
      // Workerd supports manual, not error. Never follow a redirect with the
      // verified assertion: readIdentity accepts only an unredirected 200.
      if (response.url && response.url !== endpoint) return deny('RZ-A07');
      failureCode = 'RZ-A07';
      const identity = await readIdentity(response, controller.signal);
      if (controller.signal.aborted || performance.now() >= deadline) return deny('RZ-A06');
      if (identity === null || typeof identity !== 'object' || Array.isArray(identity))
        return deny('RZ-A07');
      if (
        identity.user_uuid !== payload.sub ||
        typeof identity.email !== 'string' ||
        identity.email.toLowerCase() !== payload.email.toLowerCase()
      )
        return deny('RZ-A08');
      if (
        identity.service_token_id ||
        !(identity.service_token_status === undefined || identity.service_token_status === false)
      )
        return deny('RZ-A10');
      if (!approved.some(({ id, type }) => identity.idp?.id === id && identity.idp?.type === type))
        return deny('RZ-A09');
      return true;
    };
    try {
      // A transport that ignores abort still cannot hold a request open or
      // authorize after the complete fetch/body-parse deadline.
      return await Promise.race([lookup(), expired]);
    } catch {
      return deny(failureCode);
    } finally {
      clearTimeout(timeout);
    }
  };
}
