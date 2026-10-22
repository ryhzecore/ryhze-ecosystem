import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { parseHTML } from 'linkedom';
import ts from 'typescript';

const root = new URL('../', import.meta.url);
const html = readFileSync(
  new URL('.local/account-preview/account-preview/index.html', root),
  'utf8',
);
const source = readFileSync(
  new URL('preview/account/src/scripts/account-preview.ts', root),
  'utf8',
);
const css = readFileSync(new URL('preview/account/src/styles/account.css', root), 'utf8');
const ids = [
  'invitation',
  'sign-in',
  'sign-up',
  'verify-email',
  'forgot-password',
  'reset-password',
  'profile',
  'unavailable',
];
const compiled = ts.transpile(source.replace(/^import .+;\n/m, ''), {
  module: ts.ModuleKind.CommonJS,
  target: ts.ScriptTarget.ES2022,
});
const exports = {};
new Function('exports', 'frameMotion', compiled)(exports, () => ({ duration: 360 }));
function fixture(query = '') {
  const { document, window } = parseHTML(html);
  const location = {
    href: 'https://preview.example.test/account-preview/' + query,
    pathname: '/account-preview/',
  };
  const historyCalls = [];
  const listeners = new Map();
  const update = (kind, url) => {
    historyCalls.push({ kind, url });
    location.href = new URL(url, location.href).href;
  };
  const win = {
    location,
    history: {
      pushState: (_s, _t, url) => update('push', url),
      replaceState: (_s, _t, url) => update('replace', url),
    },
    matchMedia: () => ({ matches: true }),
    addEventListener: (name, fn) => listeners.set(name, fn),
    getComputedStyle: () => ({ getPropertyValue: () => '' }),
  };
  let focused;
  document.querySelectorAll('h2').forEach((heading) => {
    heading.focus = () => {
      focused = heading.id;
    };
  });
  exports.bootAccountPreview(document, win);
  const click = (target, extra = {}) => {
    const event = new window.Event('click', { bubbles: true, cancelable: true });
    Object.assign(
      event,
      { button: 0, metaKey: false, ctrlKey: false, shiftKey: false, altKey: false },
      extra,
    );
    target.dispatchEvent(event);
    return event;
  };
  return { document, win, historyCalls, listeners, click, focus: () => focused };
}

test('eight labelled account screens share one frame and only invitation starts visible', () => {
  const { document } = parseHTML(html);
  assert.deepEqual(
    [...document.querySelectorAll('[data-screen]')].map((el) => el.dataset.screen),
    ids,
  );
  assert.equal(document.querySelectorAll('#account-card').length, 1);
  assert.deepEqual(
    [...document.querySelectorAll('[data-screen]')]
      .filter((el) => !el.hidden)
      .map((el) => el.dataset.screen),
    ['invitation'],
  );
  for (const screen of document.querySelectorAll('[data-screen]'))
    assert.ok(document.getElementById(screen.getAttribute('aria-labelledby')));
});

test('no form accepts credentials; every input and provider/action button is disabled', () => {
  const { document } = parseHTML(html);
  assert.equal(document.querySelectorAll('form, textarea, select').length, 0);
  const inputs = [...document.querySelectorAll('input')];
  assert.ok(inputs.length >= 12);
  assert.equal(inputs.filter((input) => input.type === 'password').length, 5);
  for (const input of inputs) {
    assert.ok(input.hasAttribute('disabled'));
    assert.equal(input.getAttribute('autocomplete'), 'off');
    assert.ok(!input.hasAttribute('name'));
    assert.ok(document.querySelector(`label[for="${input.id}"]`));
    if (input.type === 'password') assert.ok(!input.getAttribute('value'));
  }
  for (const button of document.querySelectorAll('button')) {
    assert.ok(button.hasAttribute('disabled'));
    assert.equal(button.getAttribute('type'), 'button');
  }
  assert.match(html, /No accounts, emails or passwords are created or sent/);
  assert.match(html, /No email has been sent/);
  assert.match(html, /does not reserve a user ID/);
});

test('static security policy blocks connections and submissions; no external SDK or identity token', () => {
  const { document } = parseHTML(html);
  const policy = document.querySelector('[http-equiv="Content-Security-Policy"]').content;
  assert.match(policy, /connect-src 'none'/);
  assert.match(policy, /form-action 'none'/);
  assert.equal(document.querySelector('meta[name="referrer"]').content, 'no-referrer');
  assert.match(document.querySelector('meta[name="robots"]').content, /noindex/);
  for (const script of document.querySelectorAll('script')) {
    assert.match(script.src, /^\/_astro\//);
    assert.ok(!script.textContent.trim());
  }
  assert.doesNotMatch(
    source,
    /\b(fetch|XMLHttpRequest|WebSocket|localStorage|sessionStorage|indexedDB)\s*[.(]/,
  );
  assert.doesNotMatch(
    source,
    /\.cookie\s*=|\.value\b|addEventListener\(['"](?:input|change|submit)/,
  );
  assert.doesNotMatch(
    html,
    /clerk\.js|pk_(?:live|test)_|sk_(?:live|test)_|__clerk_ticket|oauth_google/,
  );
});

test('preview selection updates screen, accessible current state and heading focus', () => {
  const { document, click, focus, historyCalls } = fixture();
  for (const id of ids) {
    const link = document.querySelector(`.screen-picker [data-go="${id}"]`);
    assert.equal(click(link).defaultPrevented, true);
    assert.deepEqual(
      [...document.querySelectorAll('[data-screen]')]
        .filter((el) => !el.hidden)
        .map((el) => el.dataset.screen),
      [id],
    );
    assert.equal(document.querySelector('.screen-picker [aria-current]').dataset.go, id);
    assert.equal(focus(), `${id}-title`);
    assert.equal(historyCalls.at(-1).url, `/account-preview/?screen=${id}`);
    assert.match(
      document.getElementById('preview-status').textContent,
      /Account actions remain disabled/,
    );
  }
});

test('next control uses its current target and never claims authentication success', () => {
  const { document, click } = fixture();
  const journey = [
    'invitation',
    'sign-up',
    'verify-email',
    'profile',
    'sign-in',
    'forgot-password',
    'reset-password',
    'unavailable',
  ];
  for (let i = 1; i <= ids.length; i++) {
    const next = document.getElementById('next-screen');
    click(next);
    assert.equal(
      document.querySelector('[data-screen]:not([hidden])').dataset.screen,
      journey[i % journey.length],
    );
    assert.match(next.textContent, /^Preview /);
  }
});

test('URL screen allowlist strips unknown query values and hash without reflecting them', () => {
  for (const query of [
    '?screen=evil&ticket=secret#secret',
    '?screen=profile&__clerk_ticket=secret',
    '?returnTo=https://evil.test',
  ]) {
    const { document, historyCalls } = fixture(query);
    assert.doesNotMatch(document.documentElement.innerHTML, /secret|evil\.test/);
    assert.doesNotMatch(historyCalls[0].url, /ticket|secret|evil|returnTo/);
    assert.equal(
      document.querySelector('[data-screen]:not([hidden])').dataset.screen,
      query.includes('screen=profile') ? 'profile' : 'invitation',
    );
  }
});

test('Back and Forward resolve only allowed screens and modified clicks retain native behavior', () => {
  const { document, win, listeners, click, historyCalls } = fixture('?screen=profile');
  win.location.href = 'https://preview.example.test/account-preview/?screen=sign-in';
  listeners.get('popstate')();
  assert.equal(document.querySelector('[data-screen]:not([hidden])').dataset.screen, 'sign-in');
  const previous = historyCalls.length;
  assert.equal(
    click(document.querySelector('[data-go="profile"]'), { ctrlKey: true }).defaultPrevented,
    false,
  );
  assert.equal(historyCalls.length, previous);
});

test('motion reuses the shared frame contract and respects reduced motion and forced colors', () => {
  assert.match(source, /src\/scripts\/frame-motion/);
  assert.match(source, /prefers-reduced-motion: reduce/);
  assert.match(source, /animation\?\.cancel/);
  assert.match(css, /var\(--radius-frame\)/);
  assert.match(css, /var\(--duration-frame-enter\)/);
  assert.match(css, /forced-colors: active/);
  assert.match(css, /outline: 3px solid Highlight/);
});

test('preview build is isolated from production and contains no backend files', () => {
  const config = readFileSync(new URL('preview/account/astro.config.mjs', root), 'utf8');
  assert.match(config, /\.local\/account-preview/);
  const walk = (dir) =>
    readdirSync(dir).flatMap((name) => {
      const path = new URL(name, dir);
      return statSync(path).isDirectory() ? walk(new URL(name + '/', dir)) : [path];
    });
  for (const file of walk(new URL('.local/account-preview/', root))) {
    const path = file.pathname;
    assert.doesNotMatch(path, /\/(?:auth|hosting|gate|api|node_modules)\/|\.(?:sql|toml|map)$/);
  }
});

test('invitation primary action opens signup and explicitly describes preview only', () => {
  const { document, click, historyCalls } = fixture();
  const action = document.getElementById('invitation-continue');
  assert.equal(action.tagName, 'A');
  assert.equal(action.getAttribute('href'), '?screen=sign-up');
  assert.match(action.textContent, /Preview account setup/);
  assert.doesNotMatch(action.textContent, /Accept invitation/);
  assert.match(
    document.querySelector('[data-screen="invitation"]').textContent,
    /does not redeem an invitation or create an account/,
  );
  click(action);
  assert.equal(document.querySelector('[data-screen]:not([hidden])').dataset.screen, 'sign-up');
  assert.equal(historyCalls.at(-1).url, '/account-preview/?screen=sign-up');
  for (const input of document.querySelectorAll('input')) assert.ok(input.hasAttribute('disabled'));
});

test('skip-link modality starts hidden for touch/restored focus and responds only to Tab navigation', () => {
  const { document, listeners } = fixture();
  const send = (name, props = {}) => {
    const event = new document.defaultView.Event(name, { bubbles: true });
    Object.assign(event, props);
    document.dispatchEvent(event);
  };
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
  send('keydown', { key: 'Enter' });
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
  send('keydown', { key: 'Tab', ctrlKey: true });
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
  send('keydown', { key: 'Tab' });
  assert.equal(document.documentElement.dataset.accountInput, 'keyboard');
  send('pointerdown');
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
  send('keydown', { key: 'Tab', shiftKey: true });
  assert.equal(document.documentElement.dataset.accountInput, 'keyboard');
  send('touchstart');
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
  send('keydown', { key: 'Tab' });
  listeners.get('pageshow')();
  assert.equal(document.documentElement.dataset.accountInput, 'pointer');
});

test('skip-link CSS has no generic focus reveal and retains a no-script keyboard fallback', () => {
  assert.doesNotMatch(css, /\.skip-link:focus\s*\{/);
  assert.match(css, /\.skip-link:focus-visible\s*\{\s*transform: none;/);
  assert.match(
    css,
    /:root\[data-account-input='pointer'\] \.skip-link:focus-visible\s*\{\s*transform: translateY\(-150%\);/,
  );
});
