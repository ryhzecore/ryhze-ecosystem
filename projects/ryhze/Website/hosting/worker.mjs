import { createAgreementGate } from '../gate/index.mjs';
import { agreementGateConfig } from '../gate/config.mjs';
import { privateResponse, unavailable } from './safety.mjs';

/**
 * V0.0.8 preparation entry point. The real identity verifier is unit-tested in
 * access-identity.mjs, but no live identity/store is configured or provisioned.
 * A missing adapter is a deny condition, never an in-memory acceptance fallback.
 */
export function createStagingWorker({ config = agreementGateConfig, identity, store } = {}) {
  return {
    async fetch(request, env) {
      try {
        const gate = createAgreementGate({ config, identity, store, assets: env?.ASSETS });
        return privateResponse(await gate.fetch(request), request.method);
      } catch {
        // Never disclose claims, invitation records, configuration or errors.
        return unavailable(request.method);
      }
    },
  };
}

export default createStagingWorker();
