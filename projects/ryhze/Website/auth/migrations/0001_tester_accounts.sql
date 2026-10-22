-- Local V0.0.9 migration. No remote execution is implicit in this file.
-- Clerk owns credential/session/invitation tokens. D1 stores only application
-- authorization and profile data. Every timestamp is Unix epoch milliseconds.
PRAGMA foreign_keys = ON;

CREATE TABLE tester_principal (
  id TEXT PRIMARY KEY NOT NULL,
  provider TEXT NOT NULL CHECK (provider = 'clerk'),
  provider_subject TEXT NOT NULL CHECK (length(provider_subject) BETWEEN 1 AND 255),
  email_canonical TEXT NOT NULL CHECK (
    length(email_canonical) BETWEEN 3 AND 254 AND
    email_canonical = lower(trim(email_canonical))
  ),
  status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended', 'revoked')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch() * 1000),
  UNIQUE (provider, provider_subject),
  UNIQUE (email_canonical)
);

CREATE TABLE tester_invitation (
  id TEXT PRIMARY KEY NOT NULL,
  provider_invitation_id TEXT NOT NULL UNIQUE CHECK (length(provider_invitation_id) BETWEEN 1 AND 255),
  email_canonical TEXT NOT NULL CHECK (
    length(email_canonical) BETWEEN 3 AND 254 AND
    email_canonical = lower(trim(email_canonical))
  ),
  status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'redeemed', 'revoked')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch() * 1000),
  redeem_before INTEGER NOT NULL CHECK (typeof(redeem_before) = 'integer' AND redeem_before > created_at),
  access_expires_at INTEGER NOT NULL CHECK (typeof(access_expires_at) = 'integer' AND access_expires_at > created_at),
  redeemed_at INTEGER,
  redeemed_by TEXT REFERENCES tester_principal(id),
  CHECK (
    (redeemed_at IS NULL AND redeemed_by IS NULL AND status <> 'redeemed') OR
    (redeemed_at IS NOT NULL AND redeemed_by IS NOT NULL AND status <> 'pending')
  )
);
CREATE UNIQUE INDEX tester_one_pending_invitation_per_email
  ON tester_invitation(email_canonical) WHERE status = 'pending';

CREATE TABLE tester_membership (
  principal_id TEXT PRIMARY KEY NOT NULL REFERENCES tester_principal(id),
  invitation_id TEXT NOT NULL UNIQUE REFERENCES tester_invitation(id),
  status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended', 'revoked')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch() * 1000),
  access_expires_at INTEGER NOT NULL CHECK (typeof(access_expires_at) = 'integer' AND access_expires_at > created_at),
  revoked_at INTEGER,
  CHECK ((status = 'revoked') = (revoked_at IS NOT NULL))
);

CREATE TABLE tester_profile (
  principal_id TEXT PRIMARY KEY NOT NULL REFERENCES tester_principal(id),
  public_user_id TEXT NOT NULL UNIQUE CHECK (
    length(public_user_id) BETWEEN 1 AND 64 AND
    public_user_id = lower(public_user_id) AND
    public_user_id NOT GLOB '*[^a-z0-9._]*'
  ),
  nickname TEXT NOT NULL CHECK (
    length(nickname) BETWEEN 1 AND 80 AND instr(nickname, char(0)) = 0 AND
    nickname NOT GLOB ('*[' || char(1) || '-' || char(31) || char(127) || '-' || char(159) || ']*')
  ),
  created_at INTEGER NOT NULL DEFAULT (unixepoch() * 1000),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch() * 1000) CHECK (updated_at >= created_at)
);
