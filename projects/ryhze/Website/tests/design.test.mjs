import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { parseHTML } from 'linkedom';
const root = fileURLToPath(new URL('../', import.meta.url));
const read = (path) => readFileSync(`${root}${path}`, 'utf8');
const tokens = read('src/styles/tokens.css');
const color = (name) => tokens.match(new RegExp(`--color-${name}: (#[0-9a-f]{6})`))[1];
function luminance(hex) {
  const channels = hex
    .slice(1)
    .match(/../g)
    .map((n) => Number.parseInt(n, 16) / 255);
  return channels
    .map((v) => (v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4))
    .reduce((sum, v, i) => sum + v * [0.2126, 0.7152, 0.0722][i], 0);
}
function contrast(a, b) {
  const [lighter, darker] = [luminance(a), luminance(b)].sort((a, b) => b - a);
  return (lighter + 0.05) / (darker + 0.05);
}
test('solid theme text colors clear WCAG AA normal-text contrast', () => {
  for (const foreground of ['text', 'muted', 'subtle', 'accent', 'accent-soft']) {
    for (const background of ['bg', 'surface', 'surface-raised']) {
      assert.ok(
        contrast(color(foreground), color(background)) >= 4.5,
        `${foreground} on ${background}`,
      );
    }
  }
  for (const background of ['primary', 'primary-hover']) {
    assert.ok(contrast(color('on-primary'), color(background)) >= 4.5, `ink on ${background}`);
  }
});
test('the redesigned homepage has its new composition and unchanged brand asset', () => {
  const document = parseHTML(read('dist/index.html')).document;
  assert.ok(document.querySelector('.hero .brand-stage > .stage-symbol + .stage-copy'));
  assert.equal(
    document.querySelector('.brand-stage img')?.getAttribute('src'),
    '/brand/web/symbol-480.webp',
  );
  assert.equal(document.querySelectorAll('.discovery-card').length, 2);
  assert.equal(document.querySelectorAll('.feature-list .feature').length, 3);
  assert.equal(document.querySelectorAll('.device-list button').length, 6);
  assert.equal(document.querySelectorAll('.brand-orbit, .orbit-ring').length, 0);
  assert.match(
    document.querySelector('.access-note').textContent,
    /Downloads are not available yet/,
  );
});
test('responsive and motion source contracts cover phone-safe composition', () => {
  const css = read('src/styles/pages.css') + read('src/styles/components.css');
  assert.match(css, /max-width: 760px/);
  assert.match(css, /max-width: 390px/);
  assert.match(css, /grid-template-columns: 1fr/);
  assert.match(
    css,
    /--frame-width: min\(\s*var\(--frame-width-compact\),\s*calc\(100vw - var\(--gutter\) \* 2 - 58px\),?\s*\)/,
  );
  assert.doesNotMatch(read('src/styles/pages.css'), /white-space: nowrap|min-width: [3-9]\d\dpx/);
  assert.match(read('src/styles/base.css'), /prefers-reduced-motion: reduce/);
  assert.match(css, /background-color var\(--duration\) var\(--ease\)/);
});

test('equivalent panels share the verified 32px game-thumbnail radius', () => {
  assert.match(tokens, /--radius-panel: 32px/);
  for (const role of ['thumbnail', 'card', 'frame']) {
    assert.match(tokens, new RegExp(`--radius-${role}: var\\(--radius-panel\\)`));
  }
  const css =
    read('src/styles/pages.css') +
    read('src/styles/components.css') +
    read('src/styles/dropdown.css');
  assert.doesNotMatch(css, /border-radius: (?:14|16|20|30)px/);
  assert.match(
    read('src/styles/pages.css').match(/\.access-banner \{([^}]+)\}/)[1],
    /border-radius: var\(--radius-frame\)/,
  );
});

test('cards and buttons use stationary neutral hover elevation', () => {
  const css = read('src/styles/components.css');
  assert.doesNotMatch(css, /\.button[^{}]*:hover\s*\{[^}]*transform:/);
  assert.doesNotMatch(css.match(/\.button \{([^}]+)\}/)[1], /transform/);
  for (const selector of ['button', 'discovery-card']) {
    const hover = css.match(new RegExp(`\\.${selector}:hover \\{([^}]+)\\}`))[1];
    assert.doesNotMatch(hover, /transform|translate|scale|top:|margin/);
    assert.match(hover, /box-shadow: var\(--surface-shadow-hover\)/);
  }
  assert.match(tokens, /--color-surface: #111111/);
  assert.match(tokens, /--color-surface-raised: #181818/);
});

test('category labels have true transparent text masks and accessible fallback text', () => {
  const document = parseHTML(read('dist/index.html')).document;
  const labels = [...document.querySelectorAll('.knockout-label')];
  assert.equal(labels.length, 2);
  for (const label of labels) {
    assert.ok(label.querySelector('.sr-only')?.textContent.trim());
    assert.equal(label.querySelector('svg')?.getAttribute('aria-hidden'), 'true');
    assert.equal(label.querySelector('mask text')?.getAttribute('fill'), 'black');
    assert.ok(
      label.querySelector('rect[mask]')?.getAttribute('mask').startsWith('url(#discovery-label-'),
    );
    assert.ok(label.querySelector('.knockout-fallback')?.textContent.trim());
  }
  assert.match(read('src/styles/components.css'), /forced-colors: active/);
});

test('FAQ and platform menus use the same primitive and two standardized widths', () => {
  const document = parseHTML(read('dist/downloads/index.html')).document;
  const questions = [...document.querySelectorAll('.faq-list [data-dropdown]')];
  assert.equal(questions.length, 5);
  for (const question of questions) {
    assert.ok(question.classList.contains('dropdown--wide'));
    assert.ok(question.querySelector('summary.button.button--outline'));
    assert.ok(question.querySelector('.dropdown-frame .dropdown-content p')?.textContent.trim());
    assert.equal(question.getAttribute('open'), null);
  }
  assert.ok(document.querySelector('.site-header .dropdown--compact'));
  assert.match(tokens, /--frame-width-compact: 330px/);
  assert.match(tokens, /--frame-width-wide: 640px/);
});

test('the active website is V0.0.5 PRE-ALPHA across package, footer and metadata endpoints', () => {
  const pkg = JSON.parse(read('package.json'));
  const lock = JSON.parse(read('package-lock.json'));
  assert.equal(pkg.version, '0.0.5');
  assert.equal(lock.version, pkg.version);
  assert.equal(lock.packages[''].version, pkg.version);
  assert.deepEqual(lock.packages[''].dependencies, pkg.dependencies);
  assert.deepEqual(lock.packages[''].devDependencies, pkg.devDependencies);
  const candidate = JSON.parse(read('deployment/public/current-candidate.json'));
  const historicalRelease = JSON.parse(read('deployment/public/release.json'));
  const crosswalk = JSON.parse(read('docs/website-version-crosswalk-2026-10-06.json'));
  assert.equal(candidate.version, pkg.version);
  assert.equal(candidate.stage, 'PRE-ALPHA');
  assert.equal(candidate.status, 'unpublished-review-candidate');
  assert.equal(candidate.deployment_authorized_by_this_record, false);
  assert.equal(historicalRelease.version, '0.1.0-4');
  assert.equal(crosswalk.requested_mapping['0.0.5'], '0.0.1');
  assert.equal(crosswalk.requested_mapping['0.1.0-4'], pkg.version);
  assert.equal(crosswalk.current_candidate.version, candidate.version);
  assert.equal(crosswalk.current_candidate.stage, candidate.stage);
  assert.equal(crosswalk.current_candidate.status, candidate.status);
  assert.equal(crosswalk.historical_tag.ref, 'refs/tags/v0.0.5');
  assert.equal(crosswalk.historical_tag.target_commit, '4345eca48f5619aa7380072f182ad7f9a74df64a');
  assert.equal(crosswalk.historical_tag.owner_display_label_for_that_historical_source, '0.0.1');
  for (const path of ['dist/build-info.json', 'dist/version.json']) {
    assert.deepEqual(JSON.parse(read(path)), {
      product: 'ryhze-website',
      version: pkg.version,
      channel: 'development',
      stage: 'PRE-ALPHA',
    });
  }
  for (const page of [
    'index.html',
    'downloads/index.html',
    'entertainment/index.html',
    'games/index.html',
    'films/index.html',
    'library/index.html',
    'film-library/index.html',
    'saved/index.html',
    'race/index.html',
    'race/architecture/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    const document = parseHTML(read(`dist/${page}`)).document;
    const footer = document.querySelector('.footer-bottom').textContent;
    assert.match(footer, /Website V0\.0\.5\b/);
    assert.match(footer, /PRE-ALPHA/);
    assert.doesNotMatch(footer, /V0\.1\.0-4/);
    assert.equal(document.querySelector('.site-header .brand-stage-tag').textContent, 'PRE-ALPHA');
    const websiteVersionFields = [
      ...document.querySelectorAll('.platform-release-facts dt'),
    ].filter((term) => term.textContent.trim() === 'Website version');
    if (page === 'index.html') assert.ok(websiteVersionFields.length > 0);
    for (const term of websiteVersionFields)
      assert.equal(
        term.nextElementSibling.textContent.trim(),
        'V0.0.5 · PRE-ALPHA website, not an app release',
      );
  }
});

test('hero and shared identity copy use the exact approved uppercase headline', () => {
  const document = parseHTML(read('dist/index.html')).document;
  const headline = document.getElementById('home-title').textContent.replace(/\s+/g, ' ').trim();
  assert.equal(headline, 'ENTERTAINMENT HAS NO LIMITS');
  assert.equal(document.querySelector('title').textContent, 'Ryhze — ENTERTAINMENT HAS NO LIMITS');
  for (const page of [
    'index.html',
    'downloads/index.html',
    'entertainment/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    const html = read(`dist/${page}`);
    assert.doesNotMatch(html, /Stories to discover|Worlds to return to/i);
  }
});

test('underlined action CTAs use shared dark secondary buttons with valid link semantics', () => {
  const document = parseHTML(read('dist/index.html')).document;
  const cards = [...document.querySelectorAll('.discovery-card')];
  assert.deepEqual(
    cards.map((card) => card.querySelector('.button--secondary')?.textContent.trim()),
    ['Explore films', 'Explore games'],
  );
  for (const card of cards) {
    assert.equal(card.localName, 'a');
    assert.equal(card.querySelectorAll('a').length, 0, 'Card links cannot contain nested anchors');
    assert.equal(card.querySelector('.button--secondary').localName, 'span');
  }
  assert.equal(
    document.querySelectorAll(
      '.hero .button--secondary, .device-copy .button--secondary:not([data-platform-fallback])',
    ).length,
    1,
  );
  const downloads = parseHTML(read('dist/downloads/index.html')).document;
  assert.equal(downloads.querySelector('.back-link.button--secondary')?.getAttribute('href'), '/');
  assert.match(
    read('src/styles/components.css'),
    /\.button--secondary \{\s*background-color: var\(--color-surface-raised\)/,
  );
});

test('V0.0.7 removes superseded hero content and keeps the meaningful brand panel accessible', () => {
  const document = parseHTML(read('dist/index.html')).document;
  const hero = document.querySelector('.hero');
  assert.equal(hero.querySelectorAll('h1').length, 1);
  assert.equal(hero.querySelectorAll('a, button, summary').length, 0);
  assert.equal(hero.querySelector('.brand-stage').hasAttribute('aria-hidden'), false);
  assert.equal(hero.querySelector('.stage-symbol').getAttribute('alt'), 'Ryhze');
  assert.match(hero.querySelector('.stage-copy > h1 + p').textContent, /Games and Films/);
  assert.equal(
    hero.querySelectorAll(
      '.hero-intro, .hero-aside, .hero-bottom, .stage-topline, .stage-word, .stage-baseline',
    ).length,
    0,
  );
  assert.doesNotMatch(
    hero.textContent,
    /Welcome to Ryhze|Find your next favourite|Currently in early access|Make your dreams come true|Explore the world of Ryhze|IMAGINE\.|EXPLORE\./i,
  );
  for (const page of [
    'index.html',
    'downloads/index.html',
    'entertainment/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    assert.doesNotMatch(
      parseHTML(read(`dist/${page}`)).document.body.textContent,
      /television|series/i,
    );
  }
  const css = read('src/styles/pages.css');
  assert.match(css, /grid-template-columns: minmax\(180px, 0.85fr\) minmax\(0, 1.5fr\)/);
  assert.match(css, /grid-template-columns: minmax\(0, 1fr\)/);
  assert.match(css, /font-size: clamp\(2rem, 6.4vw, 3rem\)/);
});

test('category frames match bottom insets to top and sides without shrinking controls', () => {
  const css = read('src/styles/components.css');
  assert.match(css.match(/\.discovery-card \{([^}]+)\}/)[1], /padding: 16px;/);
  assert.match(css.match(/\.discovery-card--01 \{([^}]+)\}/)[1], /padding: 19px;/);
  assert.match(css, /min-height: 285px;\s*padding: 14px;/);
  const document = parseHTML(read('dist/index.html')).document;
  assert.deepEqual(
    [...document.querySelectorAll('.knockout-label > .sr-only')].map((label) => label.textContent),
    ['Films', 'Games'],
  );
  assert.match(css, /grid-template-columns: repeat\(2, minmax\(0, 1fr\)\)/);
});

test('the monochrome shell mounts no decorative glow layer on any public page', () => {
  for (const page of [
    'index.html',
    'downloads/index.html',
    'entertainment/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    const document = parseHTML(read(`dist/${page}`)).document;
    assert.equal(
      document.querySelectorAll('[data-ambient-background], [data-ambient-glow]').length,
      0,
    );
    assert.equal(
      document.querySelector('meta[name="theme-color"]').getAttribute('content'),
      '#000000',
    );
  }
  const css = read('src/styles/ambient.css');
  assert.match(css, /display: none/);
  assert.doesNotMatch(css, /radial-gradient|will-change/);
});

test('no invisible ambient scroll work is initialized by the production shell', () => {
  assert.doesNotMatch(read('src/scripts/ui.ts'), /initializeAmbient/);
  assert.doesNotMatch(read('src/layouts/SiteLayout.astro'), /styles\/ambient/);
});

test('mobile navigation is an initially collapsed shared compact dropdown rather than a full-width strip', () => {
  const document = parseHTML(read('dist/index.html')).document;
  const menu = document.querySelector('.mobile-navigation');
  assert.equal(menu.localName, 'details');
  assert.equal(menu.hasAttribute('open'), false);
  assert.ok(menu.classList.contains('dropdown--compact'));
  assert.ok(menu.classList.contains('dropdown--end'));
  assert.equal(menu.querySelector('summary').getAttribute('aria-label'), 'Ryhze menu');
  assert.deepEqual(
    [...menu.querySelectorAll('.dropdown-list a')].map((a) => a.getAttribute('href')),
    ['/#discover', '/downloads/', '/race/', '/entertainment/'],
  );
  assert.equal(document.querySelectorAll('.mobile-nav, .menu-toggle').length, 0);
  assert.doesNotMatch(read('src/styles/components.css'), /\.mobile-nav:not\(\[hidden\]\)/);
});

test('headers have one circular two-line menu trigger and contextual headings', () => {
  for (const page of [
    'index.html',
    'downloads/index.html',
    'entertainment/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    const document = parseHTML(read(`dist/${page}`)).document;
    const header = document.querySelector('.site-header');
    const menu = header.querySelector('.mobile-navigation, .entertainment-menu');
    assert.equal(header.querySelectorAll('.menu-trigger').length, 1);
    assert.equal(menu.querySelectorAll('.menu-trigger-lines > span').length, 2);
    assert.equal(menu.querySelector('.menu-trigger-lines').getAttribute('aria-hidden'), 'true');
    assert.equal(menu.querySelector('summary').textContent.trim(), '');
    assert.equal(menu.querySelectorAll('.dropdown-heading').length, 0);
    assert.equal(menu.hasAttribute('open'), false);
    assert.equal(header.querySelector('.brand img').getAttribute('alt'), 'Ryhze');
  }
  const css = read('src/styles/components.css');
  assert.match(css.match(/\.desktop-nav \{([^}]+)\}/)[1], /display: none/);
  assert.match(css.match(/\.mobile-navigation \{([^}]+)\}/)[1], /display: block/);
  const trigger = css.match(/\.menu-trigger\.button \{([^}]+)\}/)[1];
  assert.match(trigger, /height: 46px/);
  assert.match(trigger, /width: 46px/);
  assert.match(trigger, /border-radius: 50%/);
  assert.match(css, /@media \(min-width: 1025px\) \{\s*\.desktop-nav \{\s*display: flex;\s*\}/);
  assert.doesNotMatch(css, /\.mobile-navigation \{\s*display: none;/);
  assert.doesNotMatch(css, /min-width: 761px/);
  assert.match(read('src/scripts/navigation.ts'), /matchMedia\('\(min-width: 1025px\)'\)/);
});

test('standard headers keep all navigation inside one two-line menu without duplicate buttons or a new search', () => {
  for (const page of [
    'index.html',
    'downloads/index.html',
    'race/index.html',
    'race/architecture/index.html',
    'contact/index.html',
    'privacy/index.html',
    'legal/index.html',
    '404.html',
  ]) {
    const document = parseHTML(read(`dist/${page}`)).document;
    const header = document.querySelector('.site-header');
    const menu = header.querySelector('.mobile-navigation');
    assert.equal(header.querySelector('.desktop-nav, .platform-dropdown'), null);
    assert.equal(header.querySelectorAll('summary').length, 1);
    assert.deepEqual(
      [...menu.querySelectorAll('.dropdown-list a')].map((link) => link.textContent.trim()),
      ['Explore', 'Download', 'RACE', 'Entertainment'],
    );
    assert.equal(
      [...header.querySelectorAll('a, button')].filter(
        (control) => !control.closest('.brand, .mobile-navigation'),
      ).length,
      0,
    );
    assert.equal(menu.querySelector('a[href="/downloads/"]').textContent.trim(), 'Download');
    assert.equal(
      menu.querySelector('a[href="/downloads/"]').getAttribute('aria-current'),
      page.startsWith('downloads/') ? 'page' : null,
    );
    assert.equal(
      menu.querySelector('a[href="/entertainment/"]').textContent.trim(),
      'Entertainment',
    );
    assert.equal(menu.querySelectorAll('.menu-trigger-lines > span').length, 2);
    assert.equal(header.querySelectorAll('.menu-trigger').length, 1);
    assert.equal(header.querySelector('[data-search], [data-search-input]'), null);
  }
  const home = parseHTML(read('dist/index.html')).document;
  assert.equal(
    home
      .querySelector('#experience-platforms')
      .parentElement.querySelector('summary')
      .textContent.trim(),
    'Platforms',
  );
});

test('all framed surfaces and button variants inherit subtle resting shadows', () => {
  const components = read('src/styles/components.css');
  const pageStyles = read('src/styles/pages.css');
  assert.match(tokens, /--button-shadow: 0 3px 8px/);
  assert.match(tokens, /--surface-shadow: 0 6px 18px/);
  assert.match(components.match(/\.button \{([^}]+)\}/)[1], /box-shadow: var\(--button-shadow\)/);
  for (const selector of ['discovery-card', 'platform-card']) {
    assert.match(
      components.match(new RegExp(`\\.${selector} \\{([^}]+)\\}`))[1],
      /box-shadow: var\(--surface-shadow\)/,
    );
  }
  for (const selector of ['brand-stage', 'device-panel', 'access-banner', 'placeholder-frame']) {
    assert.match(
      pageStyles.match(new RegExp(`\\.${selector} \\{([^}]+)\\}`))[1],
      /box-shadow: var\(--surface-shadow\)/,
    );
  }
  assert.match(read('src/styles/dropdown.css'), /box-shadow: var\(--frame-shadow\)/);
  assert.doesNotMatch(components, /box-shadow: none/);
});

test('frame controls share entry, exit and easing tokens without per-page timing overrides', () => {
  assert.match(tokens, /--duration-frame-enter: 360ms/);
  assert.match(tokens, /--duration-frame-exit: 280ms/);
  assert.match(tokens, /--ease-frame: var\(--ease\)/);
  for (const path of ['src/styles/dropdown.css', 'src/styles/components.css']) {
    const css = read(path);
    assert.match(css, /transform var\(--duration-frame-exit\) var\(--ease-frame\)/);
    assert.match(css, /transition-duration: var\(--duration-frame-enter\)/);
  }
  assert.doesNotMatch(read('src/styles/entertainment.css'), /--duration-(?:dropdown|frame[^:]*):/);
  for (const path of ['src/scripts/navigation.ts', 'src/scripts/detail-frame.ts']) {
    const code = read(path);
    assert.match(code, /import \{ frameMotion \} from '\.\/frame-motion'/);
    assert.match(code, /frameMotion\(computed, (?:expanded|opening)\)/);
    assert.doesNotMatch(code, /duration:\s*\d|easing:\s*['"]/);
  }
});

test('forced-colors keyboard focus keeps a visible system outline despite component resets', () => {
  const css = read('src/styles/base.css');
  const forced = css.match(/@media \(forced-colors: active\) \{([\s\S]+?)\n\}/)?.[1];
  assert.ok(forced);
  for (const control of ['a', 'button', 'summary', 'input', 'textarea', 'select'])
    assert.match(forced, new RegExp(`(?:^|[\\s,])${control}:focus-visible`));
  assert.match(forced, /outline: 2px solid Highlight !important/);
  assert.match(forced, /outline-offset: 3px/);
  assert.doesNotMatch(forced, /forced-color-adjust: none/);
  assert.match(
    read('src/styles/components.css'),
    /\[data-title-dialog\]\[data-state='closing'\]\s*\{\s*pointer-events: none/,
  );
});

test('focus feedback is neutral and independent of normal hover shadows', () => {
  const base = read('src/styles/base.css');
  const focus = base.match(/a:focus-visible,[\s\S]+?select:focus-visible\s*\{([^}]+)\}/)?.[1];
  assert.match(focus, /outline: 2px solid var\(--color-text\)/);
  assert.match(focus, /outline-offset: 3px/);
  assert.doesNotMatch(focus, /box-shadow|color-accent|color-blue/);
  assert.match(
    base,
    /:root\[data-pointer\] :is\(a, button, summary\):focus-visible\s*\{\s*outline: none/,
  );
  assert.doesNotMatch(base, /:root\[data-pointer\][^{]*(?:input|textarea|select)/);
  assert.doesNotMatch(read('src/styles/dropdown.css'), /summary:focus-visible\s*\{[^}]*box-shadow/);
  assert.match(
    read('src/styles/entertainment.css'),
    /input:focus\s*\{\s*border-bottom-color: var\(--color-text\)/,
  );
  assert.doesNotMatch(tokens, /--focus:/);
  assert.match(tokens, /--color-accent: #ffffff/);
  assert.match(tokens, /--color-primary: #ffffff/);
});
