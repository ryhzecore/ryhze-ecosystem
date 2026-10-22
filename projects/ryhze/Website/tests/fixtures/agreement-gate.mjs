// SYNTHETIC TEST DATA ONLY. Never import this adapter into a deployment.
import { sha256 } from '../../gate/index.mjs';

export const NOW = Date.parse('2026-10-01T00:00:00Z');
export const ORIGIN = 'https://preview.example.test';
export const PRINCIPAL = { subjectRef: 'synthetic-subject-a', sessionRef: 'synthetic-session-a' };

export async function fixtureDocument(
  id,
  text = `Synthetic ${id} content for automated tests only.`,
) {
  return {
    id,
    title: `Synthetic ${id} document`,
    version: 'fixture-v1',
    approved: true,
    approvalRef: 'synthetic-approval-only',
    effectiveAt: '2026-09-30T00:00:00Z',
    text,
    sha256: await sha256(text),
  };
}

export async function fixtureConfig() {
  return {
    mode: 'enabled',
    origin: ORIGIN,
    release: '0.0.8',
    sourceCommit: 'a'.repeat(40),
    approvals: Object.fromEntries(
      [
        'operator',
        'documents',
        'privacy',
        'eligibility',
        'assetRights',
        'hosting',
        'recordRetention',
        'identity',
      ].map((key) => [key, 'synthetic-only']),
    ),
    documents: await Promise.all(['terms', 'nda', 'privacy'].map((id) => fixtureDocument(id))),
    optionalFeedback: {
      version: 'fixture-v1',
      text: 'Optional synthetic feedback permission. Leaving this unchecked does not prevent testing.',
      approvalRef: 'synthetic-only',
    },
  };
}

export async function fixtureStore(clock = () => NOW) {
  const invitations = new Map([
    [
      PRINCIPAL.subjectRef,
      {
        id: 'synthetic-invitation-a',
        revision: 'synthetic-revision-1',
        subjectRef: PRINCIPAL.subjectRef,
        status: 'active',
        eligibilityReviewed: true,
        release: '0.0.8',
        expiresAt: new Date(clock() + 24 * 60 * 60 * 1000).toISOString(),
        schedule: await fixtureDocument('schedule'),
      },
    ],
  ]);
  const records = new Map();
  const challenges = new Map();
  const key = ({ subjectRef, invitationId, packageDigest }) =>
    JSON.stringify([subjectRef, invitationId, packageDigest]);
  return {
    invitations,
    records,
    challenges,
    async getInvitation(subjectRef) {
      return structuredClone(invitations.get(subjectRef));
    },
    async getAcceptance(query) {
      return structuredClone(records.get(key(query)));
    },
    async putChallenge(challenge) {
      challenges.set(challenge.challengeHash, structuredClone(challenge));
    },
    async commitAcceptance(input) {
      // No awaits within this mutation: models the required single transaction.
      const challenge = challenges.get(input.challengeHash);
      const invite = invitations.get(input.subjectRef);
      const transactionTime = clock();
      if (
        !challenge ||
        challenge.subjectRef !== input.subjectRef ||
        challenge.sessionRef !== input.sessionRef ||
        challenge.invitationId !== input.invitationId ||
        challenge.packageDigest !== input.packageDigest ||
        challenge.invitationRevision !== invite?.revision ||
        challenge.expiresAt <= transactionTime ||
        invite?.status !== 'active' ||
        invite?.eligibilityReviewed !== true ||
        invite?.id !== input.invitationId ||
        invite?.subjectRef !== input.subjectRef ||
        invite?.release !== input.record.release ||
        JSON.stringify(invite) !== JSON.stringify(input.expectedInvitation) ||
        Date.parse(invite.expiresAt) <= transactionTime
      )
        return { status: 'rejected' };
      const previous = records.get(key(input));
      if (challenge.used) {
        return {
          status:
            previous &&
            previous.optionalFeedback?.accepted === input.record.optionalFeedback?.accepted
              ? 'already-recorded'
              : 'rejected',
        };
      }
      if (
        previous &&
        previous.optionalFeedback?.accepted !== input.record.optionalFeedback?.accepted
      )
        return { status: 'rejected' };
      challenge.used = true;
      if (!previous)
        records.set(
          key(input),
          structuredClone({ ...input.record, acceptedAt: new Date(transactionTime).toISOString() }),
        );
      return { status: previous ? 'already-recorded' : 'committed' };
    },
  };
}
