import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { scriptModule } from './helpers/script-module.mjs';
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const { initializeTitleDialog } = scriptModule('title-dialog', { AbortController });
const { initializeEntertainment } = scriptModule('entertainment', { AbortController });
function harness({
  reduced = false,
  route = 'games',
  hash = '',
  supported = true,
  tokens = {},
  asyncHistory = false,
  animated = true,
  categories = false,
} = {}) {
  const { document, window } = parseHTML(read(`dist/${route}/index.html`));
  const dialog = document.querySelector('[data-title-dialog]');
  const root = document.querySelector('[data-entertainment-root]');
  const events = new window.EventTarget();
  const media = new window.EventTarget();
  media.matches = reduced;
  let focus = document.body;
  Object.defineProperty(document, 'activeElement', { get: () => focus });
  for (const element of document.querySelectorAll('a,button,dialog'))
    element.focus = () => {
      focus = element;
    };
  Object.defineProperty(dialog, 'open', { get: () => dialog.hasAttribute('open') });
  if (supported)
    dialog.showModal = () => {
      dialog.setAttribute('open', '');
      focus = dialog.querySelector('[autofocus]');
    };
  dialog.close = () => dialog.removeAttribute('open');
  const animations = [];
  const focusFrames = new Map();
  let frameId = 0;
  if (animated)
    dialog.animate = (frames, settings) => {
      const item = {
        frames,
        settings,
        onfinish: null,
        cancelled: false,
        finished: false,
        cancel() {
          this.cancelled = true;
        },
        finish() {
          this.finished = true;
          this.onfinish?.();
        },
      };
      animations.push(item);
      return item;
    };
  const location = { pathname: `/${route}/`, search: '', hash };
  const entries = [`/${route}/${hash}`];
  let index = 0;
  const states = [null];
  const setLocation = (path) => {
    const url = new URL(path, 'https://example.test');
    Object.assign(location, { pathname: url.pathname, search: url.search, hash: url.hash });
  };
  const emit = (target, type, props = {}) => {
    const e = new window.Event(type, { bubbles: true, cancelable: true });
    Object.assign(e, { button: 0, ...props });
    target.dispatchEvent(e);
    return e;
  };
  const historyTasks = [];
  let backCalls = 0;
  const history = {
    state: null,
    pushState(state, _, path) {
      this.state = state;
      entries.splice(++index);
      entries[index] = path;
      states[index] = state;
      setLocation(path);
    },
    replaceState(state, _, path) {
      this.state = state;
      entries[index] = path;
      states[index] = state;
      setLocation(path);
    },
    back() {
      backCalls += 1;
      const traverse = () => {
        if (index > 0) {
          setLocation(entries[--index]);
          this.state = states[index];
          emit(events, 'popstate');
          emit(events, 'hashchange');
        }
      };
      if (asyncHistory) historyTasks.push(traverse);
      else traverse();
    },
    forward() {
      if (index + 1 < entries.length) {
        setLocation(entries[++index]);
        this.state = states[index];
        emit(events, 'popstate');
      }
    },
  };
  for (const target of [
    document,
    dialog,
    root,
    events,
    media,
    document.querySelector('[data-entertainment-chooser]'),
  ]) {
    const add = target.addEventListener.bind(target);
    target.addEventListener = (type, callback, options) => {
      add(type, callback);
      options?.signal?.addEventListener('abort', () => target.removeEventListener(type, callback), {
        once: true,
      });
    };
  }
  const win = {
    requestAnimationFrame: (fn) => {
      focusFrames.set(++frameId, fn);
      return frameId;
    },
    cancelAnimationFrame: (id) => focusFrames.delete(id),
    location,
    history,
    matchMedia: () => media,
    getComputedStyle: () => ({
      opacity: '0.45',
      getPropertyValue: (name) =>
        ({
          '--duration-frame-enter': '360ms',
          '--duration-frame-exit': '280ms',
          '--ease-frame': 'cubic-bezier(0.22, 0.68, 0.18, 1)',
          ...tokens,
        })[name] || '',
    }),
    addEventListener: events.addEventListener.bind(events),
  };
  const cleanup = categories
    ? initializeEntertainment(document, win)
    : initializeTitleDialog(document, win);
  const settle = () => {
    for (const a of animations) if (!a.cancelled && !a.finished) a.finish();
  };
  return {
    document,
    dialog,
    root,
    events,
    media,
    location,
    history,
    entries,
    animations,
    backCalls: () => backCalls,
    flushHistory: () => {
      while (historyTasks.length) historyTasks.shift()();
    },
    cleanup,
    settle,
    emit,
    focus: () => focus,
    resetFocus: () => {
      focus = document.body;
    },
    flushFocus: () => {
      const frames = [...focusFrames.values()];
      focusFrames.clear();
      frames.forEach((fn) => fn());
    },
    tile: document.querySelector('.entertainment-thumbnail'),
    close: dialog.querySelector('[data-close-title]'),
  };
}
test('tile opens a modal without native anchor navigation; Back restores focus and title', () => {
  const h = harness();
  const initialTitle = h.document.title;
  assert.equal(h.emit(h.tile, 'click').defaultPrevented, true);
  assert.equal(h.dialog.open, true);
  assert.equal(h.location.hash, '#game-details');
  assert.equal(h.document.body.style.overflow, 'hidden');
  assert.equal(h.focus(), h.close);
  h.settle();
  h.emit(h.close, 'click');
  h.settle();
  assert.equal(h.dialog.open, false);
  assert.equal(h.location.hash, '');
  assert.equal(h.focus(), h.tile);
  assert.equal(h.document.title, initialTitle);
  assert.notEqual(h.document.body.style.overflow, 'hidden');
});
test('Escape and outside dismissal close, but clicks inside the frame do not', () => {
  for (const way of ['escape', 'outside']) {
    const h = harness();
    h.emit(h.tile, 'click');
    h.settle();
    h.emit(h.dialog.querySelector('h2'), 'click');
    assert.equal(h.dialog.open, true);
    if (way === 'escape') assert.equal(h.emit(h.dialog, 'cancel').defaultPrevented, true);
    else h.emit(h.dialog, 'click');
    h.settle();
    assert.equal(h.dialog.open, false);
  }
});
test('Back and Forward restore the overlay without duplicate history', () => {
  const h = harness();
  h.emit(h.tile, 'click');
  h.settle();
  h.history.back();
  h.settle();
  assert.equal(h.dialog.open, false);
  h.history.forward();
  h.settle();
  assert.equal(h.dialog.open, true);
  assert.equal(h.entries.length, 2);
  h.emit(h.close, 'click');
  h.settle();
  assert.equal(h.location.hash, '');
  h.history.forward();
  h.settle();
  assert.equal(h.dialog.open, true);
});
test('Films to Games switching preserves modal title, category, and Back/Forward history', () => {
  const h = harness({ route: 'films', categories: true, reduced: true });
  const initialTitle = h.document.title;
  h.emit(h.document.querySelector('[data-category-link="games"]'), 'click');
  assert.equal(h.location.pathname, '/games/');
  assert.equal(h.root.dataset.currentCategory, 'games');
  h.emit(h.document.querySelector('.entertainment-thumbnail'), 'click');
  assert.equal(h.dialog.open, true);
  assert.equal(h.document.title, 'Larcenous Driftscape — Ryhze');
  assert.equal(h.entries.length, 3);
  h.history.back();
  assert.equal(h.dialog.open, false);
  assert.match(h.document.title, /Ryhze Games/);
  h.history.back();
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.equal(h.document.title, initialTitle);
  h.history.forward();
  h.history.forward();
  assert.equal(h.root.dataset.currentCategory, 'games');
  assert.equal(h.dialog.open, true);
  assert.equal(h.document.title, 'Larcenous Driftscape — Ryhze');
  h.emit(h.close, 'click');
  assert.equal(h.dialog.open, false);
  assert.match(h.document.title, /Ryhze Games/);
  h.cleanup();
});
test('Films search opens Games and its title in the same document, with native modified links', () => {
  const h = harness({ route: 'films', categories: true, reduced: true });
  const result = h.document.querySelector('.search-result');
  assert.equal(h.emit(result, 'click', { ctrlKey: true }).defaultPrevented, false);
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.equal(h.emit(result, 'click').defaultPrevented, true);
  assert.equal(h.root.dataset.currentCategory, 'games');
  assert.equal(h.location.pathname, '/games/');
  assert.equal(h.location.hash, '#game-details');
  assert.equal(h.dialog.open, true);
  h.emit(h.close, 'click');
  assert.equal(h.location.hash, '');
  assert.match(h.document.title, /Ryhze Games/);
  h.history.back();
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.match(h.document.title, /Ryhze Films/);
  h.cleanup();
});
test('Tommy Riley film deep links open an accurately labelled frame and dismiss on Films', () => {
  const h = harness({ route: 'films', hash: '#film-details', categories: true, reduced: true });
  assert.equal(h.dialog.open, true);
  assert.equal(h.document.title, 'Tommy Riley — Ryhze');
  assert.equal(h.dialog.getAttribute('aria-labelledby'), 'film-dialog-heading');
  const view = h.dialog.querySelector('[data-title-view="film-details"]');
  assert.equal(view.hidden, false);
  assert.equal(h.dialog.querySelector('[data-title-view="game-details"]').hidden, true);
  assert.match(view.textContent, /Lured by money, an influencer joins an underground organisation/);
  assert.match(view.textContent, /not available to watch/);
  assert.equal(view.querySelector('.title-tags'), null);
  h.emit(h.close, 'click');
  assert.equal(h.dialog.open, false);
  assert.equal(h.location.pathname, '/films/');
  assert.equal(h.location.hash, '');
  assert.match(h.document.title, /^Ryhze Films — Tommy Riley$/);
  h.cleanup();
});
test('Games search opens Tommy Riley locally and restores Film details with Back and Forward', () => {
  const h = harness({ categories: true, reduced: true });
  const result = h.document.querySelector('[data-search-title="tommy riley"]');
  assert.equal(h.emit(result, 'click', { metaKey: true }).defaultPrevented, false);
  assert.equal(h.root.dataset.currentCategory, 'games');
  assert.equal(h.emit(result, 'click').defaultPrevented, true);
  assert.equal(h.root.dataset.currentCategory, 'films');
  assert.equal(h.location.pathname, '/films/');
  assert.equal(h.location.hash, '#film-details');
  assert.equal(h.dialog.open, true);
  assert.equal(h.document.title, 'Tommy Riley — Ryhze');
  h.history.back();
  assert.equal(h.dialog.open, false);
  assert.equal(h.location.pathname, '/films/');
  assert.match(h.document.title, /^Ryhze Films/);
  h.history.back();
  assert.equal(h.root.dataset.currentCategory, 'games');
  h.history.forward();
  h.history.forward();
  assert.equal(h.dialog.open, true);
  assert.equal(h.document.title, 'Tommy Riley — Ryhze');
  h.cleanup();
});
test('film favourites synchronize between hero and modal without saving the game', () => {
  const h = harness({ route: 'films', categories: true, reduced: true });
  const heroButton = h.document.querySelector('.entertainment-copy [data-save-title]');
  h.emit(heroButton, 'click');
  h.emit(h.tile, 'click');
  const modalButton = h.dialog.querySelector('[data-save-title]');
  assert.equal(modalButton.dataset.saveTitle, 'tommy-riley');
  assert.equal(modalButton.getAttribute('aria-pressed'), 'true');
  assert.match(modalButton.getAttribute('aria-label'), /^Remove Tommy Riley/);
  h.emit(modalButton, 'click');
  assert.equal(heroButton.getAttribute('aria-pressed'), 'false');
  h.emit(h.close, 'click');
  h.emit(h.document.querySelector('[data-category-link="games"]'), 'click');
  const gameButton = h.document.querySelector('.entertainment-copy [data-save-title]');
  assert.equal(gameButton.dataset.saveTitle, 'larcenous-driftscape');
  assert.equal(gameButton.getAttribute('aria-pressed'), 'false');
  h.cleanup();
});
test('deep link opens the frame and can close without leaving the site', () => {
  const h = harness({ hash: '#game-details' });
  h.settle();
  assert.equal(h.dialog.open, true);
  h.emit(h.close, 'click');
  h.settle();
  assert.equal(h.location.hash, '');
  assert.equal(h.dialog.open, false);
  assert.equal(h.entries.length, 1);
});
test('new-tab modifiers and cross-category search links keep native navigation', () => {
  for (const props of [
    { ctrlKey: true },
    { metaKey: true },
    { shiftKey: true },
    { altKey: true },
    { button: 1 },
  ]) {
    const h = harness();
    assert.equal(h.emit(h.tile, 'click', props).defaultPrevented, false);
    assert.equal(h.dialog.open, false);
  }
  const h = harness({ route: 'films' });
  const result = h.document.querySelector('.search-result');
  assert.equal(h.emit(result, 'click').defaultPrevented, false);
  assert.equal(h.dialog.open, false);
});
test('repeated opening, interrupted closing and stale callbacks settle on the latest request', () => {
  const h = harness();
  h.emit(h.tile, 'click');
  h.emit(h.tile, 'click');
  assert.equal(h.entries.length, 2);
  const oldFinish = h.animations[0].onfinish;
  h.history.back();
  h.history.forward();
  oldFinish();
  h.settle();
  assert.equal(h.dialog.open, true);
  assert.equal(h.dialog.dataset.state, 'open');
  assert.equal(h.document.body.style.overflow, 'hidden');
});
test('reduced motion, preference change and teardown restore usable document state', () => {
  const h = harness({ reduced: true });
  h.emit(h.tile, 'click');
  assert.equal(h.animations.length, 0);
  h.emit(h.close, 'click');
  assert.equal(h.dialog.open, false);
  h.media.matches = false;
  h.emit(h.tile, 'click');
  h.media.matches = true;
  h.emit(h.media, 'change');
  assert.equal(h.dialog.dataset.state, 'open');
  h.cleanup();
  assert.equal(h.dialog.open, false);
  assert.notEqual(h.document.body.style.overflow, 'hidden');
  assert.equal(h.emit(h.tile, 'click').defaultPrevented, false);
});
test('unsupported browsers preserve native readable details', () => {
  const h = harness({ supported: false });
  assert.equal(h.root.dataset.titleDialogReady, undefined);
  assert.equal(h.emit(h.tile, 'click').defaultPrevented, false);
  assert.ok(h.document.querySelector('details#game-details'));
});
test('title frame exposes one labelled dialog, honest availability and exact visual ratio', () => {
  const h = harness();
  assert.equal(h.document.querySelectorAll('[data-title-dialog]').length, 1);
  assert.equal(h.dialog.getAttribute('aria-labelledby'), 'title-dialog-heading');
  assert.match(h.dialog.textContent, /No playable build, download or release date/);
  assert.equal(h.dialog.querySelectorAll('iframe,video,form').length, 0);
  assert.doesNotMatch(h.dialog.textContent, /Top-down|2D|Perspective/);
  const css = read('src/styles/entertainment.css');
  assert.match(css, /\.title-dialog-visual\s*\{[^}]*aspect-ratio: 16 \/ 9/s);
  assert.match(css, /calc\(\(100% - 64px\) \* 0\.4\)/);
  assert.match(css, /max-width: 480px/);
});

test('fresh fragment navigation and BFcache restore focus after the browser focus pass', () => {
  const h = harness({ hash: '#game-details' });
  h.resetFocus();
  h.flushFocus();
  assert.equal(h.focus(), h.close);
  h.resetFocus();
  h.emit(h.events, 'pageshow', { persisted: true });
  assert.equal(h.focus(), h.close);
  h.resetFocus();
  h.flushFocus();
  assert.equal(h.focus(), h.close);
  h.emit(h.close, 'click');
  h.settle();
  h.resetFocus();
  h.flushFocus();
  assert.equal(h.focus(), h.document.body);
});

test('pending restoration cannot steal focus after teardown', () => {
  const h = harness({ hash: '#game-details' });
  h.cleanup();
  h.resetFocus();
  h.flushFocus();
  assert.equal(h.focus(), h.document.body);
});

test('title frames read the same custom enter, exit and easing tokens as menu frames', () => {
  const h = harness({
    tokens: {
      '--duration-frame-enter': '.625s',
      '--duration-frame-exit': '175ms',
      '--ease-frame': 'ease-in-out',
    },
  });
  h.emit(h.tile, 'click');
  assert.equal(h.animations[0].settings.duration, 625);
  assert.equal(h.animations[0].settings.easing, 'ease-in-out');
  h.settle();
  h.emit(h.close, 'click');
  assert.equal(h.animations[1].settings.duration, 175);
  assert.equal(h.animations[1].settings.easing, 'ease-in-out');
  h.settle();
  h.cleanup();
});

test('dismissal deactivates controls before asynchronous history traversal or fade completion', () => {
  for (const way of ['close', 'escape', 'outside']) {
    const h = harness({ asyncHistory: true });
    const panel = h.dialog.querySelector('[data-title-panel]');
    h.emit(h.tile, 'click');
    h.settle();
    if (way === 'escape') h.emit(h.dialog, 'cancel');
    else h.emit(way === 'close' ? h.close : h.dialog, 'click');
    assert.equal(h.location.hash, '#game-details', 'Browser traversal has not completed yet');
    assert.equal(h.dialog.open, true, 'Native modal blocking stays until the exit has finished');
    assert.equal(h.dialog.dataset.state, 'closing');
    assert.equal(panel.hasAttribute('inert'), true);
    assert.equal(panel.getAttribute('aria-hidden'), 'true');
    assert.equal(
      h.focus(),
      h.dialog,
      'Focus leaves the dismissed controls before they become inert',
    );
    assert.equal(h.emit(h.close, 'click').defaultPrevented, true);
    for (const key of ['Tab', 'Enter', ' '])
      assert.equal(h.emit(h.close, 'keydown', { key }).defaultPrevented, true);
    h.emit(h.dialog, 'cancel');
    assert.equal(h.backCalls(), 1, 'Repeated activation cannot skip additional history');
    h.flushHistory();
    assert.equal(h.location.hash, '');
    assert.equal(
      h.animations.length,
      2,
      'Duplicate popstate/hashchange do not restart exit motion',
    );
    h.settle();
    assert.equal(h.dialog.open, false);
    assert.equal(h.focus(), h.tile);
    assert.equal(panel.hasAttribute('inert'), false);
    h.cleanup();
  }
});

test('Forward reverses a dismissing frame from its painted opacity and restores interactivity', () => {
  const h = harness();
  const panel = h.dialog.querySelector('[data-title-panel]');
  h.emit(h.tile, 'click');
  h.settle();
  h.emit(h.close, 'click');
  const exiting = h.animations.at(-1);
  const stale = exiting.onfinish;
  assert.equal(panel.hasAttribute('inert'), true);
  h.history.forward();
  assert.equal(exiting.cancelled, true);
  assert.equal(h.animations.at(-1).frames[0].opacity, '0.45');
  assert.equal(panel.hasAttribute('inert'), false);
  assert.equal(panel.hasAttribute('aria-hidden'), false);
  assert.equal(h.focus(), h.close);
  stale();
  h.settle();
  assert.equal(h.dialog.open, true);
  assert.equal(h.dialog.dataset.state, 'open');
  assert.equal(h.entries.length, 2);
  h.cleanup();
});

test('reduced-motion changes settle a closing frame and restore focus without waiting for history', () => {
  const h = harness({ asyncHistory: true });
  h.emit(h.tile, 'click');
  h.settle();
  h.emit(h.close, 'click');
  h.media.matches = true;
  h.emit(h.media, 'change');
  assert.equal(h.dialog.open, false);
  assert.equal(h.focus(), h.tile);
  assert.notEqual(h.document.body.style.overflow, 'hidden');
  assert.ok(h.animations.every((animation) => animation.cancelled));
  h.flushHistory();
  h.history.forward();
  assert.equal(h.dialog.open, true);
  assert.equal(h.dialog.dataset.state, 'open');
  assert.equal(h.focus(), h.close);
  assert.equal(h.animations.length, 2);
  h.cleanup();
});

test('zero durations and unavailable Web Animations preserve immediate modal behavior', () => {
  for (const options of [
    { tokens: { '--duration-frame-enter': '0ms', '--duration-frame-exit': '0s' } },
    { animated: false },
  ]) {
    const h = harness(options);
    h.emit(h.tile, 'click');
    assert.equal(h.dialog.open, true);
    assert.equal(h.dialog.dataset.state, 'open');
    h.emit(h.close, 'click');
    assert.equal(h.dialog.open, false);
    assert.equal(h.focus(), h.tile);
    assert.equal(h.animations.length, 0);
    h.cleanup();
  }
});
