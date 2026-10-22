import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { extname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { parseHTML } from 'linkedom';

const root = fileURLToPath(new URL('../', import.meta.url));
const dist = join(root, 'dist');
const origin = 'https://static-build.test';

function filesWithin(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = join(directory, entry.name);
    return entry.isDirectory() ? filesWithin(path) : [path];
  });
}

assert.ok(existsSync(join(dist, 'index.html')), 'Run npm run build before npm test');
const outputFiles = filesWithin(dist);
const pages = outputFiles
  .filter((path) => extname(path) === '.html')
  .map((path) => {
    const name = relative(dist, path).replaceAll('\\', '/');
    return { name, path, document: parseHTML(readFileSync(path, 'utf8')).document };
  });
const homepage = pages.find((page) => page.name === 'index.html');
const downloads = pages.find((page) => page.name === 'downloads/index.html');
// Only these retired reports are redirect documents rather than full site pages.
// Keep the normal shell checks for every other path, including any unexpected refresh page.
const retiredReportPages = new Set(['race/progress/index.html', 'race/progress/scene/index.html']);

function assertRetiredReportRedirect(name, document) {
  assert.ok(retiredReportPages.has(name));
  assert.equal(document.querySelectorAll('title').length, 1);
  assert.equal(document.querySelector('title').textContent, 'Redirecting to: /race/');
  assert.equal(document.querySelectorAll('meta[http-equiv]').length, 1);
  const refresh = document.querySelector('meta[http-equiv]');
  assert.equal(refresh.getAttribute('http-equiv'), 'refresh');
  assert.equal(refresh.getAttribute('content'), '0;url=/race/');
  assert.equal(document.querySelectorAll('link').length, 1);
  const canonical = document.querySelector('link');
  assert.equal(canonical.getAttribute('rel'), 'canonical');
  assert.equal(canonical.getAttribute('href'), 'https://ryhze.com/race/');
  assert.equal(document.querySelector('meta[name="robots"]')?.getAttribute('content'), 'noindex');
  assert.equal(document.querySelectorAll('meta').length, 2);
  assert.equal(document.querySelectorAll('a').length, 1);
  const link = document.querySelector('a');
  assert.equal(link.getAttribute('href'), '/race/');
  assert.equal(link.hasAttribute('target'), false);
  assert.equal(
    link.textContent.replace(/\s+/g, ' ').trim(),
    `Redirecting from /${name.replace('index.html', '')} to /race/`,
  );
  assert.equal(document.querySelectorAll('body').length, 1);
  assert.equal(
    document.querySelector('body').textContent.replace(/\s+/g, ' ').trim(),
    link.textContent.replace(/\s+/g, ' ').trim(),
  );
  const allowedElements = new Set(['html', 'head', 'body', 'title', 'meta', 'link', 'a', 'code']);
  for (const element of document.querySelectorAll('*')) {
    assert.ok(allowedElements.has(element.localName), `${name}: unexpected redirect content`);
    for (const attribute of element.attributes)
      assert.ok(!/^on/i.test(attribute.name), `${name}: redirect must not execute event handlers`);
  }
  assert.equal(document.querySelectorAll('[data-benchmark-row], [src], [style]').length, 0);
}

function findLocalFile(url) {
  const path = resolve(dist, `.${decodeURIComponent(url.pathname)}`);
  assert.ok(path === dist || path.startsWith(`${dist}/`), `Path escapes output: ${url}`);
  const candidates = [path, `${path}.html`, join(path, 'index.html')];
  return candidates.find((candidate) => existsSync(candidate) && statSync(candidate).isFile());
}

function assertLocalReference(value, fromPath, label) {
  assert.ok(value.trim(), `${label}: empty resource URL`);
  const base = new URL(relative(dist, fromPath).replaceAll('\\', '/'), `${origin}/`);
  const url = new URL(value, base);
  if (url.origin !== origin) return;
  const target = findLocalFile(url);
  assert.ok(target, `${label}: missing local target ${value}`);
  if (url.hash && extname(target) === '.html') {
    const document = pages.find((page) => page.path === target)?.document;
    assert.ok(
      document?.getElementById(decodeURIComponent(url.hash.slice(1))),
      `${label}: missing fragment ${value}`,
    );
  }
}

test('the static build includes home, downloads, a useful 404, and public build metadata', () => {
  assert.ok(homepage, 'Homepage is built');
  assert.ok(downloads, 'Downloads page is built');
  const notFound = pages.find((page) => page.name === '404.html');
  assert.ok(notFound?.document.querySelector('main a[href="/"]'), '404 has a way home');
  const metadata = JSON.parse(readFileSync(join(dist, 'build-info.json'), 'utf8'));
  assert.equal(metadata.product, 'ryhze-website');
  assert.equal(metadata.stage, 'PRE-ALPHA');
  assert.match(metadata.version, /^\d+\.\d+\.\d+/);
  assert.deepEqual(Object.keys(metadata).sort(), ['channel', 'product', 'stage', 'version']);
});

test('the two retired RACE reports are strict local redirect documents without report or script content', () => {
  for (const name of retiredReportPages) {
    const page = pages.find((candidate) => candidate.name === name);
    assert.ok(page, `${name}: redirect fallback is built`);
    assertRetiredReportRedirect(name, page.document);
  }
});

for (const { name, path, document } of pages) {
  test(`${name}: local links, fragments, and assets resolve in the static build`, () => {
    for (const element of document.querySelectorAll('[href], [src], [poster]')) {
      for (const attribute of ['href', 'src', 'poster']) {
        const value = element.getAttribute(attribute);
        if (value !== null)
          assertLocalReference(value, path, `${name} ${element.localName}[${attribute}]`);
      }
    }
    for (const element of document.querySelectorAll('[srcset]')) {
      for (const candidate of element.getAttribute('srcset').split(',')) {
        assertLocalReference(candidate.trim().split(/\s+/)[0], path, `${name} srcset`);
      }
    }
  });

  test(`${name}: heading hierarchy, labels, and ARIA references are internally consistent`, () => {
    if (retiredReportPages.has(name)) {
      assertRetiredReportRedirect(name, document);
      return;
    }
    assert.equal(document.documentElement.getAttribute('lang'), 'en');
    assert.ok(document.querySelector('title')?.textContent.trim());
    assert.ok(document.querySelector('meta[name="description"]')?.getAttribute('content').trim());
    assert.equal(document.querySelectorAll('main').length, 1);
    assert.equal(document.querySelectorAll('h1').length, 1);
    let previousLevel = 0;
    for (const heading of document.querySelectorAll('h1, h2, h3, h4, h5, h6')) {
      const level = Number(heading.localName.slice(1));
      assert.ok(heading.textContent.trim(), `${name}: empty heading`);
      assert.ok(
        level <= previousLevel + 1,
        `${name}: heading jumps from h${previousLevel} to h${level}`,
      );
      previousLevel = level;
    }
    const ids = [...document.querySelectorAll('[id]')].map((element) => element.id);
    assert.ok(ids.every(Boolean), 'IDs cannot be empty');
    assert.equal(new Set(ids).size, ids.length, `${name}: duplicate IDs`);
    for (const element of document.querySelectorAll(
      '[aria-controls], [aria-labelledby], [aria-describedby]',
    )) {
      for (const attribute of ['aria-controls', 'aria-labelledby', 'aria-describedby']) {
        const value = element.getAttribute(attribute);
        if (value === null) continue;
        assert.ok(value.trim(), `${name}: empty ${attribute}`);
        for (const id of value.trim().split(/\s+/)) {
          assert.ok(document.getElementById(id), `${name}: ${attribute} references missing ${id}`);
        }
      }
    }
    for (const element of document.querySelectorAll('a, button, summary')) {
      const name =
        element.getAttribute('aria-label') ||
        element.textContent.trim() ||
        element.querySelector('img[alt]')?.getAttribute('alt');
      assert.ok(name, `${element.localName} needs an accessible name`);
    }
    for (const image of document.querySelectorAll('img')) {
      assert.ok(image.hasAttribute('alt'), 'Images need explicit alternative text');
      assert.ok(
        Number(image.getAttribute('width')) > 0 && Number(image.getAttribute('height')) > 0,
        'Images reserve layout dimensions',
      );
    }
    const mobile = document.querySelector('.mobile-navigation, .entertainment-menu');
    const mobileToggle = mobile?.querySelector('summary');
    assert.equal(mobile?.hasAttribute('open'), false);
    assert.equal(mobileToggle?.getAttribute('aria-label'), 'Ryhze menu');
    assert.ok(document.getElementById(mobileToggle.getAttribute('aria-controls')));
    assert.equal(document.querySelector('.skip-link')?.getAttribute('href'), '#main');
  });

  test(`${name}: links use safe destinations and new tabs cannot access the opener`, () => {
    for (const anchor of document.querySelectorAll('a[href]')) {
      const href = anchor.getAttribute('href');
      assert.notEqual(href.trim(), '#', 'Placeholder links are not functional destinations');
      const url = new URL(href, `${origin}/`);
      assert.ok(
        ['https:', 'mailto:', 'tel:'].includes(url.protocol),
        `Unsafe link scheme: ${href}`,
      );
      assert.equal(url.username, '', `Credentials must not appear in links: ${href}`);
      assert.equal(url.password, '', `Credentials must not appear in links: ${href}`);
      if (anchor.getAttribute('target') === '_blank') {
        const rel = new Set((anchor.getAttribute('rel') || '').split(/\s+/));
        assert.ok(
          rel.has('noopener') || rel.has('noreferrer'),
          `New tab lacks opener protection: ${href}`,
        );
      }
    }
  });
}

test('all CSS asset references resolve, including self-hosted fonts', () => {
  for (const path of outputFiles.filter((path) => extname(path) === '.css')) {
    const css = readFileSync(path, 'utf8');
    for (const match of css.matchAll(/url\(\s*(["']?)(.*?)\1\s*\)/g)) {
      assertLocalReference(match[2], path, relative(dist, path));
    }
  }
  for (const family of ['Inter', 'SpaceGrotesk']) {
    assert.ok(existsSync(join(dist, 'fonts', `${family}.woff`)));
    assert.ok(existsSync(join(dist, 'fonts', `${family}-LICENSE.txt`)));
  }
});

test('the remaining experience platform dropdown preserves content, native semantics, and its unique target', () => {
  const dropdowns = [...homepage.document.querySelectorAll('.platform-dropdown[data-dropdown]')];
  assert.equal(dropdowns.length, 1, 'Only the experience section retains a platform dropdown');
  assert.equal(homepage.document.querySelector('.site-header .platform-dropdown'), null);
  const [dropdown] = dropdowns;
  assert.equal(
    dropdown.localName,
    'details',
    'Native disclosure remains usable without JavaScript',
  );
  assert.equal(dropdown.firstElementChild?.localName, 'summary');
  assert.equal(dropdown.hasAttribute('open'), false);
  const summary = dropdown.querySelector('summary');
  const panel = dropdown.querySelector('.dropdown-frame');
  assert.equal(summary.getAttribute('aria-controls'), panel.id);
  assert.equal(panel.id, 'experience-platforms');
  assert.equal(homepage.document.querySelectorAll('#experience-platforms').length, 1);
  assert.equal(dropdown.querySelectorAll('summary').length, 1);
  assert.equal(dropdown.querySelectorAll('.dropdown-frame').length, 1);
  assert.equal(
    dropdown.querySelectorAll('[role="menuitem"]').length,
    0,
    'Ordinary navigation links should retain their native semantics',
  );
  const links = [...dropdown.querySelectorAll('.dropdown-list a')];
  assert.deepEqual(
    links.map((link) => link.getAttribute('href')),
    [
      '/downloads/#windows',
      '/downloads/#linux',
      '/downloads/#android',
      '/downloads/#ios',
      '/downloads/#macos',
    ],
  );
  assert.ok(links.every((link) => /not available yet/i.test(link.textContent)));
  assert.equal(summary.textContent.trim(), 'Platforms');
  assert.equal(
    panel.querySelector('.dropdown-heading')?.textContent.trim(),
    'Choose your platform',
  );
  const footer = dropdown.querySelector('.dropdown-footer');
  assert.equal(footer?.getAttribute('href'), '/downloads/');
  assert.equal(footer?.textContent.trim(), 'See all platforms');
});

test('every native platform is unavailable and has no installer or legacy action', () => {
  for (const id of ['windows', 'linux', 'android', 'ios', 'macos']) {
    const card = downloads.document.getElementById(id);
    assert.ok(card, `${id} has a deep-linkable card`);
    assert.equal(card.querySelector('.status').textContent, 'Not available yet');
    const action = card.querySelector('.platform-download');
    assert.equal(action.localName, 'button');
    assert.equal(action.hasAttribute('disabled'), true);
    assert.equal(action.hasAttribute('href'), false);
    assert.equal(action.textContent.trim(), 'Not available yet');
    assert.equal(card.querySelectorAll('a').length, 0);
    assert.match(card.querySelector('.platform-note').textContent, /No download or release date/);
  }
  assert.equal(
    downloads.document.querySelector('.platform-card--web .button').getAttribute('href'),
    '/entertainment/',
  );
  assert.match(
    downloads.document.querySelector('.platform-card--web').textContent,
    /still being built/i,
  );
  assert.doesNotMatch(
    downloads.document.body.textContent,
    /current beta|admins|invited developers|code.signed|Build 34|Play Store release/i,
  );
});

test('all rebuild navigation stays local and reaches a built destination', () => {
  for (const { name, document } of pages) {
    for (const link of document.querySelectorAll('a[href]')) {
      assert.ok(/^[/#]/.test(link.getAttribute('href')), `${name}: ${link.getAttribute('href')}`);
    }
    if (retiredReportPages.has(name)) {
      // Astro emits one production canonical URL; its usable redirect remains same-origin.
      // All ordinary resource-resolution and safe-link tests above still apply.
      assertRetiredReportRedirect(name, document);
      continue;
    }
    assert.doesNotMatch(
      document.documentElement.outerHTML,
      /https?:\/\/(?:www\.)?ryhze\.com(?:\/|["'])/i,
    );
    assert.deepEqual(
      [...document.querySelectorAll('.site-footer nav a')].map((a) => a.getAttribute('href')),
      ['/race/', '/contact/', '/privacy/', '/legal/'],
    );
  }
});

test('supporting pages remain honest placeholders', () => {
  for (const route of ['contact', 'privacy', 'legal']) {
    const page = pages.find((page) => page.name === `${route}/index.html`);
    assert.ok(page, `${route} is built`);
    assert.equal(page.document.querySelectorAll('main .placeholder-frame').length, 1);
    assert.equal(
      page.document.querySelectorAll('main form, main input, main video, main iframe, main canvas')
        .length,
      0,
    );
    assert.equal(page.document.querySelector('main .button').getAttribute('href'), '/');
    assert.match(
      page.document.querySelector('main .status').textContent,
      /Still being built|Coming soon/,
    );
  }
});

test('source and public build contain no credentials, app bundles, or backend implementation', () => {
  const dependencyNames = Object.keys(
    JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')).dependencies || {},
  );
  assert.deepEqual(
    dependencyNames,
    ['@clerk/backend', '@clerk/clerk-js', 'astro', 'jose'],
    'Only the locked Clerk SDKs, Astro and the server-only JWT verifier are runtime dependencies',
  );
  const forbiddenOutput =
    /(?:^|\/)(?:\.env(?:\.[^/]*)?|node_modules|server|functions|gate|hosting|auth|\.git)(?:\/|$)|\.(?:apk|exe|dmg|appx|sqlite|db|sql|pem|key|map|astro|tsx?)$|\.tar\.gz$/i;
  for (const path of outputFiles) assert.doesNotMatch(relative(dist, path), forbiddenOutput);
  const credentialPatterns = [
    /-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/,
    /\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{30,}\b/,
    /\bgithub_pat_[A-Za-z0-9_]{40,}\b/,
    /\bAKIA[0-9A-Z]{16}\b/,
    /\bsk-(?:proj-)?[A-Za-z0-9_-]{30,}\b/,
    /["']?(?:api[_-]?key|access[_-]?token|client[_-]?secret|password)["']?\s*[:=]\s*["'][^"'\s]{12,}["']/i,
  ];
  const sourceFiles = filesWithin(join(root, 'src'));
  const textExtensions = new Set([
    '.astro',
    '.ts',
    '.js',
    '.mjs',
    '.html',
    '.css',
    '.json',
    '.txt',
  ]);
  for (const path of [...sourceFiles, ...outputFiles].filter((path) =>
    textExtensions.has(extname(path)),
  )) {
    const content = readFileSync(path, 'utf8');
    for (const pattern of credentialPatterns)
      assert.doesNotMatch(content, pattern, `Potential credential in ${relative(root, path)}`);
    if (path.startsWith(dist) && ['.js', '.html', '.json'].includes(extname(path))) {
      assert.doesNotMatch(
        content,
        /(?:\/workspace\/|\/Users\/|localhost:\d+|127\.0\.0\.1:\d+)/,
        `Private or development path in ${relative(root, path)}`,
      );
      assert.doesNotMatch(
        content,
        /(?:jwtVerify|createRemoteJWKSet|Cf-Access-Jwt-Assertion|CLERK_SECRET_KEY|tester_membership|ryhzeentertainment@gmail\.com)/i,
        `Server authentication or owner identity leaked into ${relative(root, path)}`,
      );
    }
    if (['.js', '.ts'].includes(extname(path))) {
      assert.doesNotMatch(
        content,
        /(?:firebase|supabase|electron|react-native|createClient\s*\(|initializeApp\s*\(|new\s+WebSocket\s*\()/i,
        `App or backend code in ${relative(root, path)}`,
      );
    }
  }
});
