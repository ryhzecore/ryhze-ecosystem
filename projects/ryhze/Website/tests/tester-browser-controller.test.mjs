import test from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { authPage } from '../auth/ui/pages.mjs';
import { bootAuth } from '../auth/ui/controller.mjs';
const origin = 'https://testers.example.test';
function fixture(t, { path = '/__auth/sign-in', search = '', clerk = {} } = {}) {
  const { document } = parseHTML(authPage({ origin, publishableKey: 'pk_live_SYNTHETIC' }, 'test'));
  const existing = {};
  for (const key of ['document', 'window', 'location', 'history', 'fetch'])
    existing[key] = globalThis[key];
  const replacements = [];
  const defaults = {
    load: async () => {},
    client: { signIn: {}, signUp: {} },
    session: null,
    ...clerk,
  };
  globalThis.document = document;
  globalThis.window = { Clerk: defaults };
  globalThis.location = {
    href: origin + path + search,
    pathname: path,
    replace: (value) => replacements.push(value),
  };
  globalThis.history = {
    replaceState: (_, __, value) => {
      replacements.push(value);
      globalThis.location.pathname = value;
    },
  };
  globalThis.fetch = async () => {
    throw new Error('Unexpected application request');
  };
  t.after(() => {
    for (const key of Object.keys(existing)) {
      if (existing[key] === undefined) delete globalThis[key];
      else globalThis[key] = existing[key];
    }
  });
  return {
    document,
    clerk: defaults,
    replacements,
    $: (id) => document.getElementById(id),
    boot: () => bootAuth({ origin }),
  };
}
test('browser controller: signup confirmation mismatch never sends credentials and clears both fields', async (t) => {
  let calls = 0;
  const f = fixture(t, {
    path: '/__auth/sign-up',
    search: '?__clerk_ticket=synthetic',
    clerk: {
      client: {
        signUp: {
          create: async () => {
            calls++;
          },
        },
        signIn: {},
      },
    },
  });
  await f.boot();
  f.$('email').value = 'test@example.test';
  f.$('password').value = 'synthetic-password-1';
  f.$('confirmation').value = 'different';
  await f.$('email-submit').onclick();
  assert.equal(calls, 0);
  assert.match(f.$('status').textContent, /do not match/);
  assert.equal(f.$('password').value, '');
  assert.equal(f.$('confirmation').value, '');
  assert.ok(f.replacements.includes('/__auth/sign-up'));
});
test('browser controller: signup sends password only to Clerk, never sends confirmation or saves ticket', async (t) => {
  const calls = [];
  const f = fixture(t, {
    path: '/__auth/sign-up',
    search: '?__clerk_ticket=synthetic',
    clerk: {
      client: {
        signIn: {},
        signUp: {
          create: async (params) => {
            calls.push(params);
            return {
              status: 'missing_requirements',
              unverifiedFields: ['email_address'],
              prepareEmailAddressVerification: async () => {},
            };
          },
        },
      },
    },
  });
  await f.boot();
  f.$('email').value = 'test@example.test';
  f.$('password').value = 'synthetic-password-1';
  f.$('confirmation').value = 'synthetic-password-1';
  await f.$('email-submit').onclick();
  assert.deepEqual(calls, [
    {
      strategy: 'ticket',
      ticket: 'synthetic',
      emailAddress: 'test@example.test',
      password: 'synthetic-password-1',
    },
  ]);
  assert.equal(f.$('verify').hidden, false);
  assert.equal(f.$('password').value, '');
});
test('browser controller: repeated sign-in clicks share one pending attempt', async (t) => {
  let release;
  let calls = 0;
  const gate = new Promise((resolve) => (release = resolve));
  const f = fixture(t, {
    clerk: {
      client: {
        signUp: {},
        signIn: {
          create: async () => {
            calls++;
            await gate;
            return { status: 'needs_identifier' };
          },
        },
      },
    },
  });
  await f.boot();
  f.$('email').value = 'test@example.test';
  f.$('password').value = 'synthetic';
  const first = f.$('email-submit').onclick();
  await f.$('email-submit').onclick();
  assert.equal(calls, 1);
  release();
  await first;
  assert.equal(f.$('email-submit').disabled, false);
});
test('browser controller: interrupted verification resumes without another password or invitation', async (t) => {
  const f = fixture(t, {
    path: '/__auth/sign-up',
    clerk: {
      client: {
        signIn: {},
        signUp: { id: 'signup_synthetic', unverifiedFields: ['email_address'] },
      },
    },
  });
  await f.boot();
  assert.equal(f.$('verify').hidden, false);
  assert.match(f.$('status').textContent, /already sent/);
});
test('browser controller: Unicode nickname and user ID preview are safe text; completed profile does not unlock content', async (t) => {
  const f = fixture(t, {
    path: '/__auth/onboarding',
    clerk: {
      session: { getToken: async () => 'synthetic-session' },
      client: { signIn: {}, signUp: {} },
    },
  });
  globalThis.fetch = async () =>
    Response.json({
      stage: 'legal-hold',
      profile: { userId: 'test.user', nickname: 'My Name 🎮 <img src=x onerror=alert(1)>' },
    });
  await f.boot();
  assert.equal(f.$('ready').hidden, false);
  assert.equal(f.$('saved-nickname').querySelector('img'), null);
  assert.match(f.$('saved-nickname').textContent, /<img/);
  assert.match(f.$('status').textContent, /waiting for approved agreements/);
  f.$('userid').value = 'My.ID_007';
  f.$('userid').oninput();
  assert.equal(f.$('userid').value, 'my.id_007');
});
test('browser controller: SDK failure exposes no working credential submission', async (t) => {
  const f = fixture(t, {
    clerk: {
      load: async () => {
        throw new Error('offline');
      },
    },
  });
  await f.boot();
  assert.equal(f.$('waiting').hidden, false);
  assert.match(f.$('status').textContent, /unavailable/);
  assert.equal(f.$('email-submit').disabled, true);
});

test('browser controller: Google button cannot start OAuth before email-only scope approval', async (t) => {
  let calls = 0;
  const f = fixture(t, {
    clerk: {
      client: {
        signUp: {},
        signIn: {
          authenticateWithRedirect: async () => {
            calls++;
          },
        },
      },
    },
  });
  await f.boot();
  assert.equal(f.$('google').disabled, true);
  await f.$('google').onclick();
  assert.equal(calls, 0);
  assert.match(f.$('status').textContent, /email-only permissions/);
});

test('browser controller: resumed signup can request a new code without resending credentials', async (t) => {
  let calls = 0;
  const f = fixture(t, {
    path: '/__auth/sign-up',
    clerk: {
      client: {
        signIn: {},
        signUp: {
          id: 'signup_synthetic',
          unverifiedFields: ['email_address'],
          prepareEmailAddressVerification: async () => {
            calls++;
          },
        },
      },
    },
  });
  await f.boot();
  await f.$('resend').onclick();
  assert.equal(calls, 1);
  assert.match(f.$('status').textContent, /new code/);
});

test('browser controller: Start again clears provider attempts so reset does not loop on reload', async (t) => {
  let resets = 0;
  const client = {
    signUp: {},
    signIn: { firstFactorVerification: { strategy: 'reset_password_email_code' } },
    resetSignIn() {
      resets++;
      this.signIn = {};
    },
    resetSignUp() {
      resets++;
      this.signUp = {};
    },
  };
  const f = fixture(t, { clerk: { client } });
  await f.boot();
  assert.equal(f.$('verify').hidden, false);
  await f.$('restart').onclick();
  assert.equal(resets, 2);
  assert.equal(f.replacements.at(-1), '/__auth/sign-in');
  await f.boot();
  assert.equal(f.$('credentials').hidden, false);
});

test('browser controller: expired-cookie recovery refreshes Clerk and restores only an approved local destination', async (t) => {
  const f = fixture(t, {
    search: '?returnTo=%2Fdownloads%2F',
    clerk: {
      session: {
        getToken: async (options) => {
          assert.equal(options.skipCache, true);
          return 'fresh-synthetic';
        },
      },
      client: { signIn: {}, signUp: {} },
    },
  });
  globalThis.fetch = async (path) => {
    assert.equal(path, '/__auth/content-status');
    return new Response(null, { status: 204 });
  };
  await f.boot();
  assert.equal(f.replacements.at(-1), '/downloads/');
});
