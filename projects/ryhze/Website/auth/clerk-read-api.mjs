// Minimal documented Clerk Backend API reads. Keep privileged transport pinned
// and redirect:manual: Workerd does not support redirect:error, and following a
// cross-origin redirect must never forward the Clerk secret to another host.
const API = 'https://api.clerk.com/v1';
export function createClerkReadClient(secretKey) {
  async function read(path) {
    const response = await fetch(API + path, {
      method: 'GET',
      redirect: 'manual',
      headers: { Authorization: `Bearer ${secretKey}`, Accept: 'application/json' },
      signal: AbortSignal.timeout(8000),
    });
    if (
      response.status !== 200 ||
      !response.headers.get('Content-Type')?.toLowerCase().includes('application/json')
    )
      throw new Error('Identity provider unavailable');
    const length = response.headers.get('Content-Length');
    if (length && Number(length) > 262144) throw new Error('Provider response too large');
    const reader = response.body?.getReader();
    if (!reader) throw new Error('Missing provider response');
    let size = 0;
    const chunks = [];
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > 262144) {
        await reader.cancel();
        throw new Error('Provider response too large');
      }
      chunks.push(value);
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
  }
  return {
    sessions: {
      async getSession(id) {
        const value = await read(`/sessions/${encodeURIComponent(id)}`);
        return {
          id: value.id,
          userId: value.user_id,
          status: value.status,
          expireAt: value.expire_at,
        };
      },
    },
    users: {
      async getUser(id) {
        const value = await read(`/users/${encodeURIComponent(id)}`);
        return {
          id: value.id,
          banned: value.banned,
          locked: value.locked,
          unsupportedConnections:
            (value.external_accounts ?? []).filter(
              (account) => !['google', 'oauth_google'].includes(account.provider),
            ).length +
            (value.enterprise_accounts ?? []).length +
            (value.saml_accounts ?? []).length,
          googleConnections: (value.external_accounts ?? [])
            .filter(
              (account) => account.provider === 'google' || account.provider === 'oauth_google',
            )
            .map((account) => ({
              emailAddress: account.email_address,
              approvedScopes: account.approved_scopes,
            })),
          primaryEmailAddressId: value.primary_email_address_id,
          emailAddresses: (value.email_addresses ?? []).map((email) => ({
            id: email.id,
            emailAddress: email.email_address,
            verification: email.verification,
          })),
        };
      },
    },
    invitations: {
      async getInvitationList({ query, status, limit }) {
        const params = new URLSearchParams({ query, status, limit: String(limit) });
        const result = await read(`/invitations?${params}`);
        const list = Array.isArray(result) ? result : result.data;
        if (!Array.isArray(list)) throw new Error('Invalid invitation result');
        return {
          data: list.map((value) => ({
            id: value.id,
            emailAddress: value.email_address,
            status: value.status,
            revoked: value.revoked,
          })),
        };
      },
    },
  };
}
