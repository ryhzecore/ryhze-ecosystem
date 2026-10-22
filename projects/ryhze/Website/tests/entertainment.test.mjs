import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { scriptModule } from './helpers/script-module.mjs';
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const source = read('src/scripts/entertainment.ts');
const context = { exports: scriptModule('entertainment', { AbortController }) };
function harness(route = 'games', hash = '') {
  const { document, window } = parseHTML(read(`dist/${route}/index.html`));
  const events = new window.EventTarget();
  const root = document.querySelector('[data-entertainment-root]');
  for (const target of [
    document,
    root,
    events,
    document.querySelector('[data-entertainment-chooser]'),
  ]) {
    const add = target.addEventListener.bind(target);
    target.addEventListener = (type, callback, opts) => {
      add(type, callback);
      opts?.signal?.addEventListener('abort', () => target.removeEventListener(type, callback), {
        once: true,
      });
    };
  }
  const location = { pathname: `/${route}/`, search: '', hash };
  const entries = [`/${route}/${hash}`];
  let index = 0;
  const setLocation = (path) => {
    const url = new URL(path, 'https://ryhze.com');
    Object.assign(location, { pathname: url.pathname, search: url.search, hash: url.hash });
  };
  const history = {
    state: null,
    pushState(state, _, path) {
      this.state = state;
      entries.splice(++index);
      entries[index] = path;
      setLocation(path);
    },
    back() {
      if (index) {
        setLocation(entries[--index]);
        events.dispatchEvent(new window.Event('popstate'));
      }
    },
    forward() {
      if (index + 1 < entries.length) {
        setLocation(entries[++index]);
        events.dispatchEvent(new window.Event('popstate'));
      }
    },
  };
  const cleanup = context.exports.initializeEntertainment(document, {
    location,
    addEventListener: events.addEventListener.bind(events),
    history,
  });
  const emit = (target, type, props = {}) => {
    const event = new window.Event(type, { bubbles: true, cancelable: true });
    Object.assign(event, { button: 0, ...props });
    target.dispatchEvent(event);
    return event;
  };
  return {
    document,
    events,
    root,
    location,
    history,
    entries,
    emit,
    cleanup,
    content: root.querySelector('[data-entertainment-content]'),
    link: (category) => document.querySelector(`[data-category-link="${category}"]`),
    buttons: () => [...root.querySelectorAll('[data-save-title]')],
  };
}
for (const [route, selected] of [
  ['entertainment', 'games'],
  ['games', 'games'],
  ['films', 'films'],
]) {
  test(`${route}: native fallback has one category, matching title and inert category templates`, () => {
    const doc = parseHTML(read(`dist/${route}/index.html`)).document;
    assert.equal(doc.querySelector('[data-entertainment-chooser]').dataset.category, selected);
    assert.equal(doc.querySelectorAll('[data-category-link][aria-current="page"]').length, 1);
    assert.equal(
      doc.querySelector('[data-category-link][aria-current="page"]').getAttribute('href'),
      `/${selected}/`,
    );
    assert.equal(doc.querySelector('[data-category-content]').dataset.categoryContent, selected);
    assert.equal(doc.querySelectorAll('template[data-category-template]').length, 2);
    assert.equal(doc.querySelectorAll('h1').length, 1);
    assert.equal(doc.querySelectorAll('iframe, video, audio, form').length, 0);
    assert.match(doc.querySelector('main').textContent, /not available|Nothing is available/i);
    assert.match(doc.title, selected === 'games' ? /Ryhze Games/ : /Ryhze Films/);
    assert.equal(doc.querySelectorAll('[data-save-title]:not([hidden])').length, 0);
  });
  test(`${route}: modified category links and native keyboard navigation remain available`, () => {
    const h = harness(route);
    const originalTitle = h.document.title;
    const originalContent = h.content.innerHTML;
    for (const category of ['films', 'games', 'films', 'films', 'games']) {
      for (const props of [
        { ctrlKey: true },
        { metaKey: true },
        { shiftKey: true },
        { altKey: true },
        { button: 1 },
      ])
        assert.equal(h.emit(h.link(category), 'click', props).defaultPrevented, false);
      for (const key of ['Enter', 'Tab', 'ArrowLeft', 'ArrowRight'])
        assert.equal(h.emit(h.link(category), 'keydown', { key }).defaultPrevented, false);
    }
    assert.equal(h.root.dataset.currentCategory, selected);
    assert.equal(h.document.title, originalTitle);
    assert.equal(h.content.innerHTML, originalContent);
    assert.equal(h.content.hasAttribute('inert'), false);
    assert.equal(h.content.hasAttribute('aria-hidden'), false);
    h.cleanup();
  });
  test(`${route}: ordinary and keyboard category activation reuse the document and history`, () => {
    const h = harness(route);
    const header = h.document.querySelector('.site-header');
    const dialog = h.document.querySelector('[data-title-dialog]');
    const categories = selected === 'games' ? ['films', 'games'] : ['games', 'films'];
    for (const category of categories) {
      const anchor = h.link(category);
      assert.equal(h.emit(anchor, 'click', { detail: 0 }).defaultPrevented, true);
      assert.equal(h.root.dataset.currentCategory, category);
      assert.equal(h.location.pathname, `/${category}/`);
      assert.equal(
        h.content.querySelector('[data-category-content]').dataset.categoryContent,
        category,
      );
      assert.equal(h.document.querySelectorAll('h1').length, 1);
      assert.equal(h.document.querySelector('.site-header'), header);
      assert.equal(h.document.querySelector('[data-title-dialog]'), dialog);
      assert.equal(h.document.querySelector('[aria-current="page"][data-category-link]'), anchor);
      assert.match(h.document.title, category === 'games' ? /Ryhze Games/ : /Ryhze Films/);
      assert.match(
        h.root.querySelector('[data-entertainment-announcement]').textContent,
        /selected/,
      );
      h.emit(anchor, 'click');
    }
    assert.equal(h.entries.length, 3, 'Repeated active category does not add history');
    h.history.back();
    assert.equal(h.root.dataset.currentCategory, categories[0]);
    h.history.back();
    assert.equal(h.root.dataset.currentCategory, selected);
    h.history.forward();
    assert.equal(h.root.dataset.currentCategory, categories[0]);
    h.cleanup();
    assert.equal(h.emit(h.link(selected), 'click').defaultPrevented, false);
  });
}
test('local category switching has no request, tracking or persistent storage machinery', () => {
  assert.doesNotMatch(source, /localStorage|sessionStorage|fetch\(|XMLHttpRequest/);
});
test('temporary favourites survive local category switches without becoming persistent', () => {
  const h = harness();
  h.emit(h.buttons()[0], 'click');
  h.emit(h.link('films'), 'click');
  h.emit(h.link('games'), 'click');
  assert.ok(h.buttons().every((button) => button.getAttribute('aria-pressed') === 'true'));
  h.emit(h.events, 'pagehide');
  assert.ok(h.buttons().every((button) => button.getAttribute('aria-pressed') === 'false'));
});
test('switching a focused category returns focus to its selected chooser link', () => {
  const h = harness();
  let focus = h.content.querySelector('.entertainment-thumbnail');
  Object.defineProperty(h.document, 'activeElement', { get: () => focus });
  for (const category of ['games', 'films'])
    h.link(category).focus = () => {
      focus = h.link(category);
    };
  h.emit(h.link('films'), 'click');
  assert.equal(focus, h.link('films'));
  assert.equal(focus.isConnected, true);
});
test('a chooser drag switches locally once and suppresses the following pointer click', () => {
  const h = harness();
  const chooser = h.document.querySelector('[data-entertainment-chooser]');
  for (const [index, category] of ['games', 'films'].entries()) {
    const link = h.link(category);
    link.getBoundingClientRect = () => ({ left: index * 84, width: 80 });
    link.click = () => h.emit(link, 'click', { detail: 0 });
  }
  const pointer = (type, x) =>
    h.emit(chooser, type, {
      pointerId: 1,
      isPrimary: true,
      clientX: x,
      clientY: 0,
    });
  pointer('pointerdown', 40);
  pointer('pointermove', 124);
  pointer('pointerup', 124);
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.equal(h.entries.length, 2);
  assert.equal(h.emit(h.link('games'), 'click', { detail: 1 }).defaultPrevented, true);
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.equal(h.entries.length, 2);
});
test('favourites are temporary, synchronized, reversible, and described visibly', () => {
  const h = harness();
  assert.ok(h.buttons().every((b) => !b.hasAttribute('hidden')));
  const note = h.document.querySelector('[data-favourites-note]');
  assert.equal(note.hidden, false);
  assert.match(note.textContent, /temporary.*reset.*leave or reload/s);
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-describedby') === note.id));
  h.emit(h.buttons()[0], 'click');
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'true'));
  assert.match(
    h.document.querySelector('[data-entertainment-announcement]').textContent,
    /^Larcenous Driftscape saved temporarily$/,
  );
  h.emit(h.buttons().at(-1), 'click');
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'false'));
});
test('leaving the document resets favourites before BFcache and new documents start unsaved', () => {
  const h = harness();
  h.emit(h.buttons()[0], 'click');
  h.emit(h.events, 'pagehide', { persisted: true });
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'false'));
  h.emit(h.events, 'pageshow', { persisted: true });
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'false'));
  assert.ok(
    harness()
      .buttons()
      .every((b) => b.getAttribute('aria-pressed') === 'false'),
  );
});
test('dismissed inert controls cannot change favourites while frames fade out', () => {
  const h = harness();
  const dialog = h.document.querySelector('[data-title-dialog]');
  dialog.setAttribute('inert', '');
  h.emit(dialog.querySelector('[data-save-title]'), 'click');
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'false'));
});
test('fallback Explore links and direct detail hashes reveal the genuine available details', () => {
  for (const selector of [
    '.entertainment-copy a[href$="#game-details"]',
    '.entertainment-thumbnail',
  ]) {
    const h = harness();
    assert.equal(h.emit(h.content.querySelector(selector), 'click').defaultPrevented, false);
    const details = h.content.querySelector('#game-details');
    assert.equal(details.open, true);
    assert.match(details.textContent, /No playable build/);
  }
  const deep = harness('games', '#game-details');
  assert.equal(deep.content.querySelector('#game-details').open, true);
});
test('dialog enhancement keeps fallback details closed and cleanup removes local listeners', () => {
  const h = harness();
  h.root.dataset.titleDialogReady = 'true';
  h.emit(h.content.querySelector('.entertainment-thumbnail'), 'click');
  assert.notEqual(h.content.querySelector('#game-details').open, true);
  h.cleanup();
  h.emit(h.buttons()[0], 'click');
  assert.ok(h.buttons().every((b) => b.getAttribute('aria-pressed') === 'false'));
});
test('chooser retains original text-only compact geometry with a white selection', () => {
  const doc = parseHTML(read('dist/games/index.html')).document;
  assert.equal(doc.querySelectorAll('.entertainment-chooser svg').length, 0);
  const css = read('src/styles/entertainment.css');
  for (const size of [80, 66, 57]) assert.match(css, new RegExp(`--chooser-width: ${size}px`));
  assert.match(css, /--chooser-gap: 4px/);
  assert.match(css, /--chooser-inset: 5px/);
  assert.doesNotMatch(css, /order: 3/);
  assert.match(css, /object-fit: contain/);
  assert.match(css, /border-radius: var\(--radius-frame\)/);
  assert.match(css, /background: var\(--color-primary\)/);
  for (const rule of css.matchAll(/[^{}]*:hover[^{}]*\{([^}]+)\}/g))
    assert.doesNotMatch(rule[1], /translate|margin|top:/);
  assert.doesNotMatch(
    read('src/components/entertainment/CategoryContent.astro'),
    /https?:|\/art\/san-coronado|iframe|video/,
  );
});

test('Games and Films omit the screenshot-identified under-card metadata and plus strip', () => {
  for (const route of ['games', 'films', 'entertainment']) {
    const doc = parseHTML(read(`dist/${route}/index.html`)).document;
    assert.equal(doc.querySelector('.entertainment-game-meta, .game-save-button'), null);
    for (const card of doc.querySelectorAll('.entertainment-game-card')) {
      assert.equal(card.querySelectorAll('[data-save-title], small').length, 0);
      assert.ok(
        card.querySelector(
          `.entertainment-thumbnail[href="#${route === 'films' ? 'film' : 'game'}-details"]`,
        ),
      );
    }
  }
  const doc = parseHTML(read('dist/games/index.html')).document;
  assert.ok(doc.querySelector('[data-title-dialog] .title-tags'));
  assert.ok(doc.querySelector('[data-title-dialog] [data-save-title]'));
});

test('opened title frames omit the Ryhze Games or Films header label and retain their real title', () => {
  for (const route of ['games', 'films', 'entertainment']) {
    const doc = parseHTML(read(`dist/${route}/index.html`)).document;
    const dialog = doc.querySelector('[data-title-dialog]');
    const header = dialog.querySelector('.detail-frame-header');
    assert.equal(header.querySelector('.detail-frame-brand, .title-dialog-brand'), null);
    assert.doesNotMatch(header.textContent, /Ryhze (?:Games|Films)/i);
    assert.ok(header.querySelector('[data-close-title]'));
    assert.ok(header.querySelector('[data-save-title]'));
    const title = doc.getElementById(dialog.getAttribute('aria-labelledby'));
    assert.equal(title.textContent, route === 'films' ? 'Tommy Riley' : 'Larcenous Driftscape');
    assert.ok(doc.querySelector('.site-header .brand'));
    assert.ok(doc.querySelector('.entertainment-chooser'));
  }
});
test('Games and Films use the same exclusive structure with accurate title and availability', () => {
  for (const [category, name, detail] of [
    ['games', 'Larcenous Driftscape', 'game'],
    ['films', 'Tommy Riley', 'film'],
  ]) {
    const doc = parseHTML(read(`dist/${category}/index.html`)).document;
    const hero = doc.querySelector('.exclusive-showcase .entertainment-hero');
    assert.deepEqual(
      [...hero.children].map((node) => node.className),
      [
        'entertainment-art entertainment-art--world',
        'entertainment-hero-content',
        'entertainment-hero-counter',
      ],
    );
    assert.equal(hero.querySelector('h1').getAttribute('aria-label'), name);
    assert.ok(
      hero.querySelector('.entertainment-art .world-artwork img[src="/brand/web/symbol-480.webp"]'),
    );
    assert.ok(hero.querySelector(`a[href="/${category}/#${detail}-details"]`));
    assert.equal(hero.querySelector('[data-save-title]').dataset.saveName, name);
    assert.ok(doc.querySelector(`#${detail}-details`));
    if (category === 'films') {
      assert.match(
        hero.querySelector('.full-description').textContent,
        /^Lured by money, an influencer joins an underground organisation\. When he regrets his allegiance, he turns against it\.$/,
      );
      assert.match(
        hero.querySelector('.entertainment-hero-footer').textContent,
        /Not film footage/,
      );
      assert.match(doc.querySelector('#film-details').textContent, /not available to watch/);
      const filmView = doc.querySelector('[data-title-view="film-details"]');
      assert.equal(filmView.querySelector('.title-tags'), null);
      assert.doesNotMatch(filmView.textContent, /San Coronado|Crime|Driving|Cast|Director/);
    }
  }
});
test('temporary favourites belong to each title and do not leak between categories', () => {
  const h = harness();
  const saved = (id) =>
    [...h.root.querySelectorAll(`[data-save-title="${id}"]`)].map((button) =>
      button.getAttribute('aria-pressed'),
    );
  h.emit(h.content.querySelector('[data-save-title]'), 'click');
  h.emit(h.link('films'), 'click');
  assert.deepEqual(saved('tommy-riley'), ['false']);
  const filmButton = h.content.querySelector('[data-save-title]');
  h.emit(filmButton, 'click');
  assert.deepEqual(saved('tommy-riley'), ['true']);
  assert.match(filmButton.getAttribute('aria-label'), /^Remove Tommy Riley/);
  assert.match(
    h.root.querySelector('[data-entertainment-announcement]').textContent,
    /^Tommy Riley saved temporarily/,
  );
  h.emit(h.link('games'), 'click');
  assert.ok(saved('larcenous-driftscape').every((state) => state === 'true'));
  h.emit(h.content.querySelector('[data-save-title]'), 'click');
  h.emit(h.link('films'), 'click');
  assert.deepEqual(saved('tommy-riley'), ['true']);
  assert.ok(saved('larcenous-driftscape').every((state) => state === 'false'));
  h.emit(h.events, 'pagehide');
  assert.ok(h.buttons().every((button) => button.getAttribute('aria-pressed') === 'false'));
});
