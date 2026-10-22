#!/usr/bin/env node
import { pathToFileURL } from 'node:url';
import { canonicalInvitationEmail } from '../auth/profile-policy.mjs';

/** A reviewable parameterized plan, never an implicit grant or a public endpoint.
 * The assistant must have a bounded owner request before applying any future
 * invitation, suspension or revocation through authorized provider tooling. */
export function planTesterAction(input, now = Date.now()) {
  if (typeof input?.requestRef !== 'string' || !input.requestRef.trim())
    throw new Error('An explicit owner-request reference is required.');
  const email = canonicalInvitationEmail(input.email);
  if (!email) throw new Error('A valid exact invitation email is required.');
  if (input.action === 'invite') {
    const redeemBefore = Date.parse(input.redeemBefore);
    const accessExpiresAt = Date.parse(input.accessExpiresAt);
    const origin = new URL(input.origin);
    if (
      origin.protocol !== 'https:' ||
      origin.origin !== input.origin ||
      origin.port ||
      origin.username ||
      origin.password
    )
      throw new Error('An approved HTTPS tester origin is required.');
    if (
      !Number.isSafeInteger(redeemBefore) ||
      !Number.isSafeInteger(accessExpiresAt) ||
      redeemBefore <= now ||
      accessExpiresAt <= now ||
      redeemBefore > accessExpiresAt
    )
      throw new Error(
        'Explicit future redemption and access expiries are required, with redemption no later than access expiry.',
      );
    const expiresInDays = Math.ceil((redeemBefore - now) / 86400000);
    if (expiresInDays > 30)
      throw new Error('This pre-alpha plan supports invitations up to 30 days.');
    return {
      dryRun: true,
      release: '0.0.9',
      action: 'invite',
      requestRef: input.requestRef,
      clerk: {
        operation: 'invitations.createInvitation',
        params: {
          emailAddress: email,
          expiresInDays,
          notify: false,
          ignoreExisting: false,
          redirectUrl: origin.origin + '/__auth/sign-up',
        },
      },
      database: {
        sql: 'INSERT INTO tester_invitation (id,provider_invitation_id,email_canonical,redeem_before,access_expires_at) VALUES (?,?,?,?,?)',
        params: [
          '<new opaque UUID>',
          '<verified Clerk invitation ID>',
          email,
          redeemBefore,
          accessExpiresAt,
        ],
      },
      notes: [
        'No operation has run. No email has been sent. The Clerk invitation link must only be delivered to the specifically approved recipient.',
        'Apply requires verified Clerk production invite-only settings, the approved D1 binding, and legal/privacy readiness.',
        'Create the provider invitation without notification, record its exact ID durably, and verify both before separately authorized delivery.',
        'If the provider outcome is uncertain, reconcile by exact email/ID before retrying. Never blindly create a second invitation.',
        'Seven days is a proposed redemption window only; this plan uses the explicitly supplied expiry.',
      ],
    };
  }
  if (input.action === 'suspend')
    return {
      dryRun: true,
      release: '0.0.9',
      action: 'suspend',
      requestRef: input.requestRef,
      database: {
        sql: "UPDATE tester_membership SET status='suspended' WHERE status='active' AND principal_id IN (SELECT id FROM tester_principal WHERE email_canonical=?)",
        params: [email],
      },
      notes: [
        'No operation has run. Verify denial on direct assets after applying through the approved D1 operator route.',
      ],
    };
  if (input.action === 'revoke')
    return {
      dryRun: true,
      release: '0.0.9',
      action: 'revoke',
      requestRef: input.requestRef,
      database: {
        transaction: [
          {
            sql: "UPDATE tester_invitation SET status='revoked' WHERE email_canonical=?",
            params: [email],
          },
          {
            sql: "UPDATE tester_membership SET status='revoked',revoked_at=unixepoch()*1000 WHERE principal_id IN (SELECT id FROM tester_principal WHERE email_canonical=?)",
            params: [email],
          },
        ],
      },
      notes: [
        'No operation has run. Revoke any still-pending corresponding Clerk invitation by its verified ID, and verify D1 membership denial on direct assets.',
      ],
    };
  throw new Error('Supported actions: invite, suspend, revoke.');
}

export function parseArguments(args) {
  const options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!/^--[a-z-]+$/.test(args[i]) || args[i + 1] === undefined || args[i + 1].startsWith('--'))
      throw new Error('Use named option/value pairs.');
    const key = args[i].slice(2).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    if (
      !['action', 'email', 'requestRef', 'origin', 'redeemBefore', 'accessExpiresAt'].includes(
        key,
      ) ||
      Object.hasOwn(options, key)
    )
      throw new Error(
        'Unknown or duplicate option. This preparation CLI cannot apply remote changes.',
      );
    options[key] = args[i + 1];
  }
  return options;
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const options = parseArguments(process.argv.slice(2));
    if (!options.action)
      throw new Error(
        'Dry-run only. Example: --action invite --email tester@example.test --request-ref approved-request-id --origin https://testers.example.test --redeem-before 2026-10-08T12:00:00Z --access-expires-at 2026-10-15T12:00:00Z',
      );
    console.log(JSON.stringify(planTesterAction(options), null, 2));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
