import { readAuthConfig, validateAuthConfig } from '../auth/config.mjs';
import { createClerkIdentity } from '../auth/clerk-identity.mjs';
import { createTesterStore } from '../auth/d1-store.mjs';
import { authPage, authCsp } from '../auth/ui/pages.mjs';
import { isSameOriginMutation, readSmallJson } from '../auth/csrf.mjs';
import clerkSdk from '../auth/generated/clerk-sdk.mjs';
import { privateResponse, unavailable } from './safety.mjs';
import { agreementGateConfig } from '../gate/config.mjs';
import { createAgreementGate } from '../gate/index.mjs';
import { attachDocumentSession, safeReturnPath } from '../auth/document-session.mjs';

const json = (code, status = 200, extra = {}) => Response.json({ code, ...extra }, { status });
const authPages = new Set([
  '/__auth/sign-in',
  '/__auth/sign-up',
  '/__auth/callback',
  '/__auth/onboarding',
]);
const authApis = new Set([
  '/__auth/session',
  '/__auth/redeem',
  '/__auth/profile',
  '/__auth/content-status',
]);
const nonce = () =>
  Array.from(crypto.getRandomValues(new Uint8Array(18)), (b) =>
    b.toString(16).padStart(2, '0'),
  ).join('');
const redirect = (path) => new Response(null, { status: 303, headers: { Location: path } });

/** No runtime flag can manufacture legal approval. The default runtime uses the
 * still-draft source configuration and no legal store, so content remains closed.
 * A future, separately reviewed legal adapter must satisfy the existing gate. */
export function createTesterWorker({
  config,
  identity,
  store,
  legalConfig = agreementGateConfig,
  legalStore,
} = {}) {
  validateAuthConfig(config);
  if (!identity || !store) throw new Error('Server identity and D1 are required');
  return {
    async fetch(request, env) {
      try {
        const url = new URL(request.url);
        if (url.origin !== config.origin)
          return privateResponse(json('wrong_origin', 403), request.method);
        if (/%|\/\//.test(url.pathname))
          return privateResponse(json('not_found', 404), request.method);
        if (authPages.has(url.pathname) || url.pathname === '/__auth/clerk.js') {
          if (!['GET', 'HEAD'].includes(request.method))
            return privateResponse(json('method_not_allowed', 405), request.method);
          if (url.pathname === '/__auth/clerk.js') {
            return privateResponse(
              new Response(clerkSdk, {
                headers: { 'Content-Type': 'text/javascript; charset=utf-8' },
              }),
              request.method,
            );
          }
          const value = nonce();
          return privateResponse(
            new Response(authPage(config, value), {
              headers: {
                'Content-Type': 'text/html; charset=utf-8',
                'Content-Security-Policy': authCsp(config, value),
              },
            }),
            request.method,
          );
        }
        if (url.pathname.startsWith('/__auth/') && !authApis.has(url.pathname))
          return privateResponse(json('not_found', 404), request.method);
        const person = await identity.resolve(request);
        if (!person) {
          const navigation =
            request.method === 'GET' &&
            request.headers.get('Accept')?.includes('text/html') &&
            !authApis.has(url.pathname);
          return privateResponse(
            navigation
              ? redirect(
                  '/__auth/sign-in?returnTo=' + encodeURIComponent(safeReturnPath(url.pathname)),
                )
              : json('authentication_required', 401),
            request.method,
          );
        }
        if (request.method === 'POST' && authApis.has(url.pathname)) {
          if (!isSameOriginMutation(request, config.origin))
            return privateResponse(json('invalid_request', 403), request.method);
          if (url.pathname === '/__auth/redeem') {
            const body = await readSmallJson(request, []);
            if (!body) return privateResponse(json('invalid_request', 400), request.method);
            if (await store.resolveMembership(person))
              return privateResponse(json('invitation_confirmed'), request.method);
            const pending = await store.resolvePendingInvitation(person);
            if (!pending)
              return privateResponse(json('invitation_unavailable', 403), request.method);
            const record = await identity.acceptedInvitation(pending.providerInvitationId);
            const result = await store.redeemProviderInvitation(person, record);
            return privateResponse(
              result.status === 'accepted'
                ? json('invitation_confirmed')
                : json('invitation_unavailable', 403),
              request.method,
            );
          }
          if (url.pathname === '/__auth/profile') {
            const body = await readSmallJson(request, ['userId', 'nickname']);
            if (!body || Object.keys(body).length !== 2)
              return privateResponse(json('profile_invalid', 400), request.method);
            if (!(await store.resolveMembership(person)))
              return privateResponse(json('membership_required', 403), request.method);
            const result = await store.saveProfile(person, body);
            return privateResponse(
              result.status === 'saved'
                ? json('profile_saved')
                : result.status === 'conflict'
                  ? json('profile_conflict', 409)
                  : json('profile_invalid', 400),
              request.method,
            );
          }
          return privateResponse(json('method_not_allowed', 405), request.method);
        }
        if (
          !['GET', 'HEAD'].includes(request.method) &&
          !(request.method === 'POST' && url.pathname === '/__agreements/accept')
        )
          return privateResponse(json('method_not_allowed', 405), request.method);
        const member = await store.resolveMembership(person);
        if (url.pathname === '/__auth/session') {
          if (!member) {
            const pending = await store.resolvePendingInvitation(person);
            return privateResponse(
              pending
                ? json('onboarding', 200, { stage: 'invitation' })
                : json('invitation_unavailable', 403),
              request.method,
            );
          }
          return privateResponse(
            json('onboarding', 200, {
              stage:
                member.userId && member.nickname
                  ? legalConfig?.mode === 'enabled' && legalStore
                    ? 'ready-for-agreements'
                    : 'legal-hold'
                  : 'profile',
              profile: member.userId ? { userId: member.userId, nickname: member.nickname } : null,
            }),
            request.method,
          );
        }
        if (!member) return privateResponse(json('membership_required', 403), request.method);
        if (!member.userId || !member.nickname)
          return privateResponse(json('profile_required', 403), request.method);
        if (!env?.ASSETS || typeof env.ASSETS.fetch !== 'function')
          return unavailable(request.method);
        const safeAssets = {
          fetch: async (assetRequest) => {
            // Membership is re-read immediately before each asset delivery, rather
            // than accepting a cached frontend claim or a prior onboarding check.
            if (!(await store.resolveMembership(person))) return json('membership_required', 403);
            if (url.pathname === '/__auth/content-status')
              return new Response(null, { status: 204 });
            const headers = new Headers(assetRequest.headers);
            for (const name of [...headers.keys()])
              if (/^(?:authorization|cookie|cf-access-.+|x-auth-.+)$/i.test(name))
                headers.delete(name);
            const response = await env.ASSETS.fetch(
              new Request(assetRequest.url, { method: assetRequest.method, headers }),
            );
            const location = response.headers.get('Location');
            if (location && new URL(location, url).origin !== config.origin)
              return unavailable(request.method);
            return response;
          },
        };
        const gate = createAgreementGate({
          config: legalConfig,
          store: legalStore,
          identity: {
            resolve: async (currentRequest) => {
              const current = await identity.resolve(currentRequest);
              if (
                !current ||
                current.subject !== person.subject ||
                current.sessionId !== person.sessionId ||
                current.email !== person.email
              )
                return null;
              const currentMember = await store.resolveMembership(current);
              if (!currentMember || currentMember.principalId !== member.principalId) return null;
              return { subjectRef: currentMember.principalId, sessionRef: current.sessionId };
            },
          },
          assets: safeAssets,
        });
        const gatedResponse = await gate.fetch(request);
        return privateResponse(
          request.method === 'HEAD'
            ? gatedResponse
            : await attachDocumentSession(gatedResponse, config, url.pathname),
          request.method,
        );
      } catch {
        return unavailable(request.method);
      }
    },
  };
}

// The exported production entry point accepts no test identities, synthetic
// stores, local-development keys, bearer invitation tokens or client grant flags.
export default {
  async fetch(request, env) {
    try {
      const config = readAuthConfig(env);
      const identity = createClerkIdentity(config);
      const store = createTesterStore(env?.TESTER_DB);
      return await createTesterWorker({ config, identity, store }).fetch(request, env);
    } catch {
      return unavailable(request.method);
    }
  },
};
