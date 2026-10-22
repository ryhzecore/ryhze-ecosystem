import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { cascade } from './helpers/css-cascade.mjs';
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const home = parseHTML(read('dist/index.html')).document;
const pages = [
  'index.html',
  'downloads/index.html',
  'entertainment/index.html',
  'games/index.html',
  'films/index.html',
  'contact/index.html',
  'privacy/index.html',
  'legal/index.html',
  '404.html',
].map((name) => parseHTML(read(`dist/${name}`)).document);

test('actual CSS cascade preserves shared text-button padding and touch height at every breakpoint', () => {
  for (const width of [320, 390, 768, 960, 1024, 1440]) {
    for (const document of pages) {
      for (const button of document.querySelectorAll(
        '.button:not(.menu-trigger):not(.entertainment-icon-button)',
      )) {
        const style = cascade(button, width);
        if (width <= 1000 && button.matches('.entertainment-hero-content [data-save-title]')) {
          assert.equal(style.width, '52px');
          assert.equal(style['min-height'], '52px');
          assert.equal(style['padding-left'], '0');
          assert.ok(button.getAttribute('aria-label'));
          continue;
        }
        assert.equal(style['padding-top'], '14px', `${width}: ${button.textContent.trim()}`);
        assert.equal(style['padding-bottom'], '14px');
        assert.equal(style['padding-left'], '24px');
        assert.equal(style['padding-right'], '24px');
        assert.equal(style['min-height'], '52px');
      }
    }
    assert.equal(cascade(home.querySelector('.mobile-navigation'), width).display, 'block');
    const circle = cascade(home.querySelector('.menu-trigger'), width);
    assert.equal(circle.width, '46px');
    assert.equal(circle.height, '46px');
    assert.equal(circle['padding-left'], '0');
    assert.equal(circle['border-radius'], '50%');
  }
});

test('CTA gaps preserve shared spacing and the referenced exclusive hero spacing', () => {
  for (const width of [320, 390, 760, 761, 768, 960, 961, 1024, 1440]) {
    for (const document of pages) {
      for (const element of document.querySelectorAll(
        '.card-copy > .button, .device-copy .actions, .platform-card .button, .entertainment-copy .actions, .entertainment-game-info .actions',
      )) {
        assert.equal(
          cascade(element, width)['margin-top'],
          element.matches('.entertainment-hero-content .actions')
            ? '20px'
            : element.matches('.entertainment-copy .actions') && width > 760
              ? '30px'
              : '24px',
          `${width}: ${element.className}`,
        );
      }
      for (const paragraph of document.querySelectorAll(
        '.placeholder-description, .not-found p:not(.eyebrow)',
      )) {
        assert.equal(cascade(paragraph, width)['margin-bottom'], '24px');
      }
    }
    for (const card of home.querySelectorAll('.discovery-card')) {
      const style = cascade(card, width);
      const featured = card.classList.contains('discovery-card--01');
      const inset = width <= 960 ? 14 : featured ? 19 : 16;
      assert.equal(style['padding-left'], `${inset}px`);
      assert.equal(style['padding-right'], `${inset}px`);
      assert.equal(style['padding-top'], `${inset}px`);
      assert.equal(style['padding-bottom'], `${inset}px`);
    }
  }
});

test('all three experience items have shared 32px framed surfaces and requested factual headings', () => {
  assert.deepEqual(
    [...home.querySelectorAll('.feature h3')].map((node) => node.textContent),
    ['Explore Games and Films', 'Platform availability', 'See what comes next'],
  );
  for (const feature of home.querySelectorAll('.feature')) {
    for (const width of [320, 390, 768, 1440]) {
      const style = cascade(feature, width);
      assert.equal(style['border-radius'], '32px');
      assert.equal(style.border, '1px solid #323232');
      assert.ok(style['box-shadow']);
      assert.equal(style['background-color'], '#181818');
      assert.ok(Number.parseFloat(style['padding-left']) >= 24);
      assert.equal(style['min-width'], '0');
    }
  }
});

test('phone and compact-tablet menu cascade yields equal viewport gutters without overflow', () => {
  const panel = home.querySelector('.mobile-navigation > .dropdown-frame');
  for (const width of [320, 360, 374, 375, 390, 430, 600, 760, 768, 820, 960, 1024]) {
    const style = cascade(panel, width);
    assert.equal(cascade(panel.parentElement, width).position, 'static');
    assert.equal(cascade(home.querySelector('.header-inner'), width).position, 'relative');
    assert.equal(style['inset-inline-start'], '0');
    assert.equal(style['inset-inline-end'], '0');
    assert.equal(style.width, '100%');
    assert.equal(style['max-width'], '100%');
    // Arithmetic from the actual container and --gutter contract, not rendered bounds.
    const gutter =
      width <= 350 ? 16 : width <= 700 ? 22 : Math.max(20, Math.min(width * 0.045, 88));
    const containerWidth = width - 2 * gutter;
    const left = (width - containerWidth) / 2;
    const right = width - left - containerWidth;
    assert.ok(Math.abs(left - right) < 0.001);
    assert.ok(left >= 16 && right >= 16);
    assert.ok(containerWidth > 0 && containerWidth <= width);
  }
  assert.equal(
    cascade(home.querySelector('.platform-dropdown .dropdown-frame'), 390).width,
    '330px',
    'Other compact menus keep their existing width',
  );
});

test('skip-link CSS keeps non-keyboard focus visually clipped and preserves a no-JS fallback', () => {
  const css = read('src/styles/base.css');
  assert.doesNotMatch(css, /\.skip-link:focus\s*\{/);
  assert.match(css, /\.skip-link:not\(\[data-skip-enhanced\]\):focus-visible/);
  assert.match(css, /\.skip-link\[data-keyboard-focus\]:focus/);
  for (const document of pages) {
    const link = document.querySelector('.skip-link');
    assert.equal(link.getAttribute('href'), '#main');
    assert.equal(document.getElementById('main').getAttribute('tabindex'), '-1');
    const style = cascade(link, 390);
    assert.equal(style.opacity, '0');
    assert.equal(style['clip-path'], 'inset(50%)');
    assert.equal(style['pointer-events'], 'none');
    assert.notEqual(style.display, 'none');
  }
});

test('every footer paints the shared opaque black surface across the page width', () => {
  for (const document of pages) {
    const footer = document.querySelector('.site-footer');
    assert.equal(footer.parentElement, document.body);
    assert.ok(footer.querySelector(':scope > .container'));
    for (const width of [320, 486, 1177, 1920]) {
      assert.equal(cascade(footer, width)['background-color'], '#000000');
      assert.equal(cascade(footer, width).opacity, undefined);
    }
  }
});

test('Entertainment icon controls override shared text-button sizing without losing circular touch targets', () => {
  const doc = parseHTML(read('dist/games/index.html')).document;
  for (const width of [320, 375, 390, 760, 761, 1440]) {
    for (const button of doc.querySelectorAll(
      '.entertainment-toolbar .entertainment-icon-button, .entertainment-toolbar .menu-trigger',
    )) {
      const style = cascade(button, width);
      const size = width <= 760 ? '44px' : '52px';
      assert.equal(style.width, size);
      assert.equal(style.height, size);
      assert.equal(style['min-height'], size);
      assert.equal(style['padding-left'], '0');
      assert.equal(style['padding-top'], '0');
    }
  }
});

test('both exclusive banners keep contained 16:9 artwork and copy in the same row at every breakpoint', () => {
  for (const [category, title, hash] of [
    ['games', 'Larcenous Driftscape', '#game-details'],
    ['films', 'Tommy Riley', '#film-details'],
  ]) {
    const doc = parseHTML(read(`dist/${category}/index.html`)).document;
    for (const width of [320, 375, 390, 760, 1000, 1001, 1177, 1920]) {
      const hero = doc.querySelector('.exclusive-showcase .entertainment-hero');
      const content = hero.querySelector('.entertainment-hero-content');
      assert.notEqual(cascade(content, width)['overflow-y'], 'auto');
      assert.equal(cascade(content, width)['grid-area'].replaceAll(' ', ''), '1/1');
      assert.equal(cascade(content, width).position, 'relative');
      assert.ok(content.querySelector('.actions'));
      assert.ok(content.querySelector('.entertainment-hero-footer'));
      const art = hero.querySelector('.world-artwork');
      const image = art.querySelector('img');
      assert.equal(cascade(art, width)['aspect-ratio'].replaceAll(' ', ''), '16/9');
      assert.equal(cascade(image, width)['object-fit'], 'contain');
      const tile = doc.querySelector('.entertainment-thumbnail');
      assert.equal(cascade(tile, width)['aspect-ratio'].replaceAll(' ', ''), '16/9');
    }
    const card = doc.querySelector('.entertainment-game-card');
    assert.equal(card.querySelector('.entertainment-game-info'), null);
    assert.equal(card.querySelector('.thumbnail-title').textContent, title);
    assert.equal(card.querySelector('.entertainment-thumbnail').getAttribute('href'), hash);
    assert.equal(card.querySelector('.entertainment-game-meta, .game-save-button'), null);
  }
});

test('screenshot platform choices are separated, filled secondary buttons at every breakpoint', () => {
  const list = home.querySelector('.device-list');
  const buttons = [...list.querySelectorAll('button')];
  assert.equal(buttons.length, 6);
  for (const width of [320, 360, 390, 430, 768, 960, 1024, 1440]) {
    assert.equal(cascade(list, width).gap, '16px');
    assert.equal(cascade(list, width)['min-width'], '0');
    for (const button of buttons) {
      assert.ok(button.matches('.button.button--secondary'));
      assert.equal(button.getAttribute('aria-haspopup'), 'dialog');
      assert.equal(button.hasAttribute('disabled'), false);
      assert.equal(button.querySelector('small'), null);
      assert.equal(button.lastElementChild.getAttribute('aria-hidden'), 'true');
      assert.equal(button.lastElementChild.textContent.trim(), '→');
      const style = cascade(button, width);
      assert.equal(style.width, '100%');
      assert.equal(style['background-color'], '#181818');
      assert.equal(style['border-color'], '#323232');
      assert.equal(style['border-radius'], '999px');
      assert.ok(style['box-shadow']);
      assert.equal(style['min-height'], '52px');
      assert.equal(style['padding-left'], '24px');
      assert.equal(style['padding-right'], '24px');
      assert.equal(style['white-space'], 'normal');
      assert.equal(style.color, '#f7f7f7');
      assert.equal(cascade(button.querySelector('span'), width)['min-width'], '0');
    }
  }
  assert.doesNotMatch(read('src/styles/pages.css'), /\.device-list[^{}]*\{[^}]*border-bottom:/);
});
