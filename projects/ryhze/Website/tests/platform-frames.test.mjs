import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { scriptModule } from './helpers/script-module.mjs';
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const { initializePlatformFrames } = scriptModule('platform-frames', { AbortController });
const { initializeTitleDialog } = scriptModule('title-dialog', { AbortController });
const { initializeEntertainment } = scriptModule('entertainment', { AbortController });
function harness({
  reduced = false,
  route = '',
  hash = '',
  supported = true,
  tokens = {},
  asyncHistory = false,
  animated = true,
  titleOrder = null,
  categories = false,
} = {}) {
  const { document, window } = parseHTML(
    read(route ? `dist/${route}/index.html` : 'dist/index.html'),
  );
  const dialog = document.querySelector('[data-platform-dialog]');
  const dialogs = [...document.querySelectorAll('dialog')];
  const root = document.querySelector('body');
  const events = new window.EventTarget();
  const media = new window.EventTarget();
  media.matches = reduced;
  let focus = document.body;
  Object.defineProperty(document, 'activeElement', { get: () => focus });
  for (const element of document.querySelectorAll('a,button,dialog,summary'))
    element.focus = () => {
      focus = element;
    };
  for (const modal of dialogs) {
    Object.defineProperty(modal, 'open', { get: () => modal.hasAttribute('open') });
    if (supported)
      modal.showModal = () => {
        modal.setAttribute('open', '');
        focus = modal.querySelector('[autofocus]');
      };
    modal.close = () => modal.removeAttribute('open');
  }
  const animations = [];
  const focusFrames = new Map();
  let frameId = 0;
  if (animated)
    for (const modal of dialogs)
      modal.animate = (frames, settings) => {
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
  const location = { pathname: route ? `/${route}/` : '/', search: '', hash };
  const entries = [`${route ? `/${route}/` : '/'}${hash}`];
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
  const targets = [document, ...dialogs, events, media];
  if (categories)
    targets.push(
      document.querySelector('[data-entertainment-root]'),
      document.querySelector('[data-entertainment-chooser]'),
    );
  for (const target of targets) {
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
  const cleanups = [];
  if (titleOrder === 'first') cleanups.push(initializeTitleDialog(document, win));
  cleanups.push(
    categories ? initializeEntertainment(document, win) : initializePlatformFrames(document, win),
  );
  if (titleOrder === 'last') cleanups.push(initializeTitleDialog(document, win));
  const cleanup = () => cleanups.forEach((stop) => stop());
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
    navigateHash: (hash) => {
      history.pushState(null, '', `${location.pathname}${location.search}${hash}`);
      emit(events, 'hashchange');
    },
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
    tile: document.querySelector('.device-list [data-platform-open=platform-windows]'),
    close: dialog.querySelector('[data-close-platform]'),
  };
}

function visible(h) {
  return [...h.dialog.querySelectorAll('[data-platform-view]')].filter((view) => !view.hidden);
}
function open(h, id = 'windows') {
  const button = h.document.querySelector(`.device-list [data-platform-open="platform-${id}"]`);
  h.emit(button, 'click');
  h.settle();
  return button;
}
test('Platforms Back and Forward reopen after a local Games/Films category change', () => {
  const h = harness({ route: 'films', categories: true, reduced: true });
  h.emit(h.document.querySelector('[data-category-link="games"]'), 'click');
  const trigger = h.document.querySelector('.entertainment-menu a[href="/downloads/"]');
  h.emit(trigger, 'click');
  assert.equal(h.location.pathname, '/games/');
  assert.equal(h.location.hash, '#platforms');
  assert.equal(h.dialog.open, true);
  h.history.back();
  assert.equal(h.dialog.open, false);
  assert.match(h.document.title, /Ryhze Games/);
  h.history.forward();
  assert.equal(h.dialog.open, true);
  assert.equal(visible(h)[0].dataset.platformView, 'platforms');
  assert.equal(h.document.title, 'Choose your platform — Ryhze');
  h.emit(h.close, 'click');
  assert.equal(h.dialog.open, false);
  assert.equal(h.location.hash, '');
  assert.match(h.document.title, /Ryhze Games/);
  h.cleanup();
});
test('Entertainment frame category links close the modal, clear its hash, and stay in the document', () => {
  for (const category of ['games', 'films']) {
    const h = harness({
      route: 'games',
      hash: '#platform-entertainment',
      categories: true,
      reduced: true,
    });
    const root = h.document.querySelector('[data-entertainment-root]');
    const header = h.document.querySelector('.site-header');
    const link = h.dialog.querySelector(`a[href="/${category}/"]`);
    assert.equal(h.dialog.open, true);
    assert.equal(h.emit(link, 'click').defaultPrevented, true);
    assert.equal(h.location.pathname, `/${category}/`);
    assert.equal(h.location.hash, '');
    assert.equal(root.dataset.currentCategory, category);
    assert.equal(h.dialog.open, false);
    assert.notEqual(h.document.body.style.overflow, 'hidden');
    assert.equal(h.document.querySelector('.site-header'), header);
    assert.match(h.document.title, category === 'games' ? /Ryhze Games/ : /Ryhze Films/);
    h.history.back();
    assert.equal(root.dataset.currentCategory, 'games');
    assert.equal(h.dialog.open, true);
    assert.equal(visible(h)[0].dataset.platformView, 'platform-entertainment');
    assert.equal(h.document.title, 'Entertainment — Ryhze');
    h.emit(h.close, 'click');
    assert.equal(h.dialog.open, false);
    assert.equal(h.location.pathname, '/games/');
    assert.equal(h.location.hash, '');
    assert.match(h.document.title, /Ryhze Games/);
    h.cleanup();
  }
});
test('all five screenshot rows are enabled native buttons opening their same-page frame', () => {
  for (const id of ['windows', 'linux', 'android', 'ios', 'macos']) {
    const h = harness();
    const originalTitle = h.document.title;
    const button = open(h, id);
    assert.equal(button.localName, 'button');
    assert.equal(button.hidden, false);
    assert.equal(h.location.pathname, '/');
    assert.equal(h.location.hash, `#platform-${id}`);
    assert.equal(visible(h).length, 1);
    assert.equal(visible(h)[0].dataset.platformView, `platform-${id}`);
    assert.equal(h.focus(), h.close);
    h.emit(h.close, 'click');
    h.settle();
    assert.equal(h.dialog.open, false);
    assert.equal(h.focus(), button);
    assert.equal(h.document.title, originalTitle);
    assert.equal(h.location.hash, '');
    h.cleanup();
  }
});
test('the homepage Entertainment button opens its frame and keeps Games/Films links native', () => {
  const h = harness();
  const button = h.document.querySelector(
    '.device-copy [data-platform-open="platform-entertainment"]',
  );
  assert.ok(button, 'The button is in the homepage Platforms/Entertainment control group');
  assert.equal(button.localName, 'button');
  assert.equal(button.hidden, false);
  assert.equal(h.emit(button, 'click').defaultPrevented, true);
  h.settle();
  assert.equal(h.location.pathname, '/');
  assert.equal(h.location.hash, '#platform-entertainment');
  assert.equal(visible(h).length, 1);
  const view = visible(h)[0];
  assert.equal(view.dataset.platformView, 'platform-entertainment');
  assert.match(view.textContent, /Still being built/);
  assert.match(view.textContent, /does not require an app download/);
  assert.equal(h.focus(), h.close);
  for (const [href, label] of [
    ['/games/', 'Explore Games'],
    ['/films/', 'Explore Films'],
  ]) {
    const link = view.querySelector(`a[href="${href}"]`);
    assert.ok(link);
    assert.equal(link.textContent.trim(), label);
    for (const props of [{}, { ctrlKey: true }, { metaKey: true }, { button: 1 }])
      assert.equal(h.emit(link, 'click', props).defaultPrevented, false);
  }
  assert.equal(h.entries.length, 2, 'Native navigation is not replaced by another frame state');
  h.emit(h.close, 'click');
  h.settle();
  assert.equal(h.dialog.open, false);
  assert.equal(h.focus(), button);
  h.cleanup();
});
test('switching between platform and title frames preserves document state in either listener order', () => {
  for (const titleOrder of ['first', 'last']) {
    for (const reduced of [false, true]) {
      const h = harness({ route: 'games', titleOrder, reduced });
      const originalTitle = h.document.title;
      h.document.body.style.overflow = 'auto';
      const titleDialog = h.document.querySelector('[data-title-dialog]');
      const titleClose = titleDialog.querySelector('[data-close-title]');
      h.emit(h.document.querySelector('a[href="/downloads/"]'), 'click');
      h.settle();
      assert.equal(h.dialog.open, true);
      h.navigateHash('#game-details');
      assert.equal(h.dialog.open, false, 'The previous native modal is released synchronously');
      assert.equal(titleDialog.open, true);
      h.settle();
      h.flushFocus();
      assert.equal(h.document.body.style.overflow, 'hidden');
      assert.equal(h.document.title, 'Larcenous Driftscape — Ryhze');
      assert.equal(h.focus(), titleClose);
      h.emit(titleClose, 'click');
      h.settle();
      assert.equal(titleDialog.open, false);
      assert.equal(h.document.querySelectorAll('dialog[open]').length, 0);
      assert.equal(h.document.body.style.overflow, 'auto');
      assert.equal(h.document.title, originalTitle);

      h.emit(h.document.querySelector('.entertainment-thumbnail'), 'click');
      const staleTitleFinish = h.animations.at(-1)?.onfinish;
      h.navigateHash('#platform-ios');
      assert.equal(titleDialog.open, false, 'Switching the other way also releases the old modal');
      assert.equal(h.dialog.open, true);
      staleTitleFinish?.();
      h.settle();
      h.flushFocus();
      assert.equal(h.document.body.style.overflow, 'hidden');
      assert.equal(h.document.title, 'iOS — Ryhze');
      assert.equal(h.focus(), h.close);
      assert.equal(visible(h)[0].dataset.platformView, 'platform-ios');
      h.emit(h.close, 'click');
      h.settle();
      assert.equal(h.document.querySelectorAll('dialog[open]').length, 0);
      assert.equal(h.document.body.style.overflow, 'auto');
      assert.equal(h.document.title, originalTitle);
      h.cleanup();
    }
  }
});
test('Download opens review information, not a file or another page', () => {
  for (const id of ['windows', 'linux', 'android', 'ios', 'macos']) {
    const h = harness();
    open(h, id);
    const button = h.dialog.querySelector(`[data-platform-open=download-${id}]`);
    h.emit(button, 'click');
    h.settle();
    assert.equal(h.location.hash, `#download-${id}`);
    assert.equal(h.location.pathname, '/');
    assert.equal(visible(h).length, 1);
    const review = visible(h)[0];
    assert.equal(review.dataset.platformView, 'download');
    assert.equal(
      h.dialog.querySelector('h2').textContent,
      `${h.dialog.querySelector(`[data-platform-view=platform-${id}]`).dataset.frameTitle} download`,
    );
    for (const phrase of [
      'Latest app version',
      'No public release yet',
      'Licensing',
      'Terms',
      'Privacy',
      'No verified public installer',
      'Website version',
    ])
      assert.ok(review.textContent.includes(phrase), phrase);
    assert.equal(review.querySelectorAll('a[href],form,input,iframe,[download]').length, 0);
    assert.equal(review.querySelector('button').disabled, true);
    assert.equal(h.focus(), h.close);
    h.emit(h.close, 'click');
    h.settle();
    assert.equal(visible(h)[0].dataset.platformView, `platform-${id}`);
    h.emit(h.close, 'click');
    h.settle();
    assert.equal(h.dialog.open, false);
    h.cleanup();
  }
});
test('platform chooser and nested details have reversible Back/Forward history', () => {
  const h = harness();
  h.emit(h.document.querySelector('.device-list [data-platform-open=platforms]'), 'click');
  h.settle();
  assert.equal(h.location.hash, '#platforms');
  h.emit(h.dialog.querySelector('[data-platform-open=platform-android]'), 'click');
  h.settle();
  h.emit(h.dialog.querySelector('[data-platform-open=download-android]'), 'click');
  h.settle();
  assert.equal(h.entries.length, 4);
  for (const view of ['platform-android', 'platforms']) {
    h.history.back();
    h.settle();
    assert.equal(
      visible(h)[0].dataset.platformView,
      view.startsWith('download-') ? 'download' : view,
    );
    assert.equal(h.location.hash, `#${view}`);
  }
  h.history.back();
  h.settle();
  assert.equal(h.dialog.open, false);
  for (const view of ['platforms', 'platform-android', 'download-android']) {
    h.history.forward();
    h.settle();
    assert.equal(
      visible(h)[0].dataset.platformView,
      view.startsWith('download-') ? 'download' : view,
    );
    assert.equal(h.location.hash, `#${view}`);
  }
  assert.equal(h.entries.length, 4);
  h.cleanup();
});
test('repeated Download activations do not duplicate history and interrupted history returns correct view', () => {
  const h = harness();
  open(h);
  const download = h.dialog.querySelector('[data-platform-open=download-windows]');
  h.emit(download, 'click');
  const stale = h.animations.at(-1).onfinish;
  h.emit(download, 'click');
  assert.equal(h.entries.length, 3);
  h.history.back();
  h.history.forward();
  stale();
  h.settle();
  assert.equal(visible(h)[0].dataset.platformView, 'download');
  assert.equal(h.dialog.querySelector('h2').textContent, 'Windows download');
  assert.equal(h.dialog.dataset.state, 'open');
  h.cleanup();
});
test('Escape, outside and Back dismiss immediately with asynchronous history and no double traversal', () => {
  for (const way of ['escape', 'outside', 'back']) {
    const h = harness({ asyncHistory: true });
    open(h);
    if (way === 'escape') h.emit(h.dialog, 'cancel');
    else h.emit(way === 'back' ? h.close : h.dialog, 'click');
    const panel = h.dialog.querySelector('[data-platform-panel]');
    assert.equal(panel.hasAttribute('inert'), true);
    assert.equal(h.focus(), h.dialog);
    h.emit(h.close, 'click');
    h.emit(h.dialog, 'cancel');
    assert.equal(h.backCalls(), 1);
    h.flushHistory();
    h.settle();
    assert.equal(h.dialog.open, false);
    assert.equal(h.focus(), h.tile);
    h.cleanup();
  }
});
test('deep links and unknown fragments do not leave stale modal state', () => {
  for (const hash of ['#platforms', '#platform-ios', '#download-linux']) {
    const h = harness({ hash });
    h.settle();
    assert.equal(h.dialog.open, true);
    assert.equal(
      visible(h)[0].dataset.platformView,
      hash.startsWith('#download-') ? 'download' : hash.slice(1),
    );
    h.emit(h.close, 'click');
    h.settle();
    assert.equal(h.location.hash, '');
    assert.equal(h.entries.length, 1);
    assert.equal(h.dialog.open, false);
    h.cleanup();
  }
  for (const hash of ['#platform-invented', '#download-windows-extra', '#game-details']) {
    const h = harness({ hash });
    assert.equal(h.dialog.open, false);
    h.cleanup();
  }
});
test('existing Platforms links are enhanced in-place while modified clicks remain native', () => {
  const h = harness();
  const link = h.document.querySelector('a[href="/downloads/#windows"]');
  for (const props of [
    { ctrlKey: true },
    { metaKey: true },
    { shiftKey: true },
    { altKey: true },
    { button: 1 },
  ]) {
    assert.equal(h.emit(link, 'click', props).defaultPrevented, false);
    assert.equal(h.dialog.open, false);
  }
  assert.equal(h.emit(link, 'click').defaultPrevented, true);
  h.settle();
  assert.equal(h.location.hash, '#platform-windows');
  h.cleanup();
});
test('unsupported native dialog keeps the readable old route and hides enhancement-only buttons', () => {
  const h = harness({ supported: false });
  assert.equal(h.document.body.dataset.platformDialogReady, undefined);
  assert.equal(h.tile.hidden, true);
  const fallback = h.document.querySelector('.device-list [data-platform-fallback]');
  assert.equal(fallback.getAttribute('href'), '/downloads/');
  assert.equal(h.emit(fallback, 'click').defaultPrevented, false);
  h.cleanup();
});
test('reduced motion and missing animations keep the nested review usable', () => {
  for (const options of [
    { reduced: true },
    { animated: false },
    { tokens: { '--duration-frame-enter': '0ms', '--duration-frame-exit': '0s' } },
  ]) {
    const h = harness(options);
    open(h);
    h.emit(h.dialog.querySelector('[data-platform-open=download-windows]'), 'click');
    assert.equal(visible(h)[0].dataset.platformView, 'download');
    assert.equal(h.dialog.querySelector('h2').textContent, 'Windows download');
    assert.equal(h.animations.length, 0);
    h.emit(h.close, 'click');
    assert.equal(visible(h)[0].dataset.platformView, 'platform-windows');
    h.cleanup();
    assert.equal(h.dialog.open, false);
    assert.notEqual(h.document.body.style.overflow, 'hidden');
    assert.equal(h.tile.hidden, true);
  }
});
test('hidden release fields are not surfaced as made-up release versions or acceptance', () => {
  const h = harness();
  for (const review of h.dialog.querySelectorAll('[data-download-review]')) {
    assert.doesNotMatch(review.textContent, /I agree|Accept terms|\.apk|\.exe|\.dmg|0\.0\.1/);
    assert.match(review.textContent, /approved.*not available yet|Approved.*not available yet/);
  }
  assert.equal(h.dialog.querySelectorAll('[aria-labelledby]').length, 0);
  assert.equal(h.dialog.getAttribute('aria-labelledby'), 'platform-frame-heading');
  h.cleanup();
});

test('closing a frame opened from a collapsing platform dropdown returns focus to its summary', () => {
  const h = harness();
  const link = h.document.querySelector('.platform-dropdown a[href="/downloads/#windows"]');
  const dropdown = link.closest('[data-dropdown]');
  const frame = dropdown.querySelector('.dropdown-frame');
  h.emit(link, 'click');
  h.settle();
  frame.setAttribute('inert', '');
  h.emit(h.close, 'click');
  h.settle();
  assert.equal(h.focus(), dropdown.querySelector('summary'));
  h.cleanup();
});
