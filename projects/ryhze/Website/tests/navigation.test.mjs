import assert from 'node:assert/strict';
import { setMaxListeners } from 'node:events';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { scriptModule } from './helpers/script-module.mjs';

class BrowserAbortController extends AbortController {
  constructor() {
    super();
    // A shared controller can legitimately manage more than ten UI listeners.
    setMaxListeners(0, this.signal);
  }
}

const { initializeNavigation } = scriptModule('navigation', {
  AbortController: BrowserAbortController,
});

/**
 * Linkedom supplies HTML parsing and event bubbling. It does not implement
 * layout, keyboard default actions, focus tracking, native details toggles,
 * matchMedia, Web Animations, or abortable event listeners. Supply those primitives
 * here; tests exercise the unmodified production controller against built HTML.
 * Animation mocks verify timing/state/cancellation, not rendered interpolation.
 * This cannot establish visual layout, actual Tab order, or browser/AT behavior.
 */
function createHarness(
  html = readFileSync(new URL('../dist/index.html', import.meta.url), 'utf8'),
  { animate = false, reduced = false, tokens = {} } = {},
) {
  const { document, window } = parseHTML(html);
  let activeElement = document.body;
  Object.defineProperty(document, 'activeElement', { get: () => activeElement });
  const frames = [];
  const eventTargets = [...document.querySelectorAll('*'), document];
  const windowEvents = new window.EventTarget();
  const media = new window.EventTarget();
  const reducedMotion = new window.EventTarget();
  reducedMotion.matches = reduced;
  eventTargets.push(windowEvents, media, reducedMotion);
  const animations = [];
  if (animate) {
    for (const frame of document.querySelectorAll('.dropdown-frame')) {
      frame.animate = (keyframes, options) => {
        const animation = {
          keyframes,
          options,
          cancelled: false,
          onfinish: null,
          cancel() {
            this.cancelled = true;
          },
          finish() {
            this.onfinish?.();
          },
        };
        animations.push(animation);
        return animation;
      };
    }
  }
  let listenerCount = 0;
  for (const target of eventTargets) {
    const add = target.addEventListener.bind(target);
    const remove = target.removeEventListener.bind(target);
    target.addEventListener = (type, callback, options) => {
      if (options?.signal?.aborted) return;
      add(type, callback, options);
      listenerCount += 1;
      options?.signal?.addEventListener(
        'abort',
        () => {
          remove(type, callback);
          listenerCount -= 1;
        },
        { once: true },
      );
    };
    if (target.nodeType === 1)
      target.focus = () => {
        activeElement = target;
      };
  }
  for (const details of document.querySelectorAll('details')) {
    Object.defineProperty(details, 'open', {
      get: () => details.hasAttribute('open'),
      set: (open) => {
        if (open) details.setAttribute('open', '');
        else details.removeAttribute('open');
      },
    });
  }
  const win = {
    addEventListener: windowEvents.addEventListener.bind(windowEvents),
    matchMedia: (query) => {
      if (query === '(prefers-reduced-motion: reduce)') return reducedMotion;
      assert.equal(query, '(min-width: 1025px)');
      return media;
    },
    getComputedStyle: () => ({
      opacity: '0.6',
      transform: 'matrix(1, 0, 0, 1, 0, -3)',
      getPropertyValue: (name) =>
        ({
          '--duration-frame-enter': '360ms',
          '--duration-frame-exit': '280ms',
          '--ease-frame': 'cubic-bezier(0.22, 0.68, 0.18, 1)',
          ...tokens,
        })[name] || '',
    }),
    requestAnimationFrame: (callback) => {
      frames.push(callback);
      return frames.length;
    },
  };
  const emit = (target, type, properties = {}) => {
    const event = new window.Event(type, {
      bubbles: !['toggle', 'pageshow', 'change'].includes(type),
      cancelable: true,
    });
    Object.assign(event, properties);
    target.dispatchEvent(event);
    return event;
  };
  const flushFrames = () => {
    while (frames.length) frames.shift()(0);
  };
  const openDropdown = (dropdown) => {
    dropdown.open = true;
    emit(dropdown, 'toggle');
  };
  const cleanup = initializeNavigation(document, win);
  return {
    document,
    emit,
    flushFrames,
    openDropdown,
    cleanup,
    win,
    windowEvents,
    media,
    reducedMotion,
    animations,
    listenerCount: () => listenerCount,
    dropdowns: [
      document.querySelector('.platform-dropdown[data-dropdown]'),
      document.querySelector('.mobile-navigation[data-dropdown]'),
    ].filter(Boolean),
    toggle: document.querySelector('.mobile-navigation > summary'),
    mobile: document.querySelector('.mobile-navigation'),
  };
}

test('navigation can be imported without browser globals and tolerates absent optional controls', () => {
  assert.equal(typeof initializeNavigation, 'function');
  const harness = createHarness('<!doctype html><html><body><main></main></body></html>');
  assert.doesNotThrow(() => harness.emit(harness.document.body, 'click'));
  assert.doesNotThrow(() => harness.emit(harness.document, 'keydown', { key: 'Escape' }));
  assert.doesNotThrow(harness.cleanup);
});

test('opening one dropdown closes the others and clicking inside its panel leaves it open', () => {
  const h = createHarness();
  for (const dropdown of h.dropdowns) {
    h.openDropdown(dropdown);
    assert.deepEqual(
      h.dropdowns.map((item) => item.open),
      h.dropdowns.map((item) => item === dropdown),
    );
  }
  h.openDropdown(h.dropdowns[0]);
  h.emit(h.dropdowns[0].querySelector('.dropdown-heading'), 'click');
  assert.equal(
    h.dropdowns[0].open,
    true,
    'Non-link content within the dropdown does not dismiss it',
  );
  h.cleanup();
});

test('FAQ answers use the shared disclosure behavior without stealing reading keys', () => {
  const html = readFileSync(new URL('../dist/downloads/index.html', import.meta.url), 'utf8');
  const h = createHarness(html);
  const faq = h.document.querySelector('.faq-list [data-dropdown]');
  assert.ok(faq);
  const trigger = faq.querySelector('summary');
  trigger.focus();
  h.emit(trigger, 'click');
  assert.equal(faq.open, true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'true');
  for (const key of ['ArrowDown', 'ArrowUp', 'Home', 'End']) {
    assert.equal(h.emit(trigger, 'keydown', { key }).defaultPrevented, false);
  }
  h.openDropdown(h.dropdowns[0]);
  assert.equal(faq.open, false, 'Opening another disclosure closes the FAQ panel');
  h.emit(trigger, 'click');
  h.emit(faq.querySelector('.dropdown-content'), 'click');
  assert.equal(faq.open, true, 'Answer text remains readable without dismissing itself');
  h.emit(trigger, 'keydown', { key: 'Escape' });
  assert.equal(faq.open, false);
  assert.equal(h.document.activeElement, trigger);
  h.emit(trigger, 'click');
  h.emit(h.document.querySelector('main'), 'click');
  assert.equal(faq.open, false, 'Outside click dismisses the answer');
  h.cleanup();
});

test('wide FAQ answers share reduced-motion and interrupted-close handling', () => {
  const html = readFileSync(new URL('../dist/downloads/index.html', import.meta.url), 'utf8');
  const h = createHarness(html, { animate: true });
  const faq = h.document.querySelector('.faq-list [data-dropdown]');
  const trigger = faq.querySelector('summary');
  const frame = faq.querySelector('.dropdown-frame');
  h.emit(trigger, 'click');
  assert.equal(h.animations.length, 1);
  h.animations[0].finish();
  h.emit(trigger, 'click');
  assert.equal(frame.hasAttribute('inert'), true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'false');
  h.emit(trigger, 'click');
  assert.equal(frame.hasAttribute('inert'), false);
  assert.equal(trigger.getAttribute('aria-expanded'), 'true');
  h.reducedMotion.matches = true;
  h.emit(h.reducedMotion, 'change');
  h.emit(trigger, 'click');
  assert.equal(faq.open, false);
  h.cleanup();
});

test('dropdown keyboard navigation includes the footer and wraps in both directions', () => {
  const h = createHarness();
  const dropdown = h.dropdowns[0];
  const trigger = dropdown.querySelector('summary');
  const links = [...dropdown.querySelectorAll('.dropdown-frame a')];
  assert.equal(links.length, 6);
  h.openDropdown(h.mobile);
  trigger.focus();
  assert.equal(h.emit(trigger, 'keydown', { key: 'ArrowDown' }).defaultPrevented, true);
  assert.equal(dropdown.open, true);
  assert.equal(h.mobile.open, false);
  assert.equal(h.document.activeElement, links[0]);
  h.emit(links[0], 'keydown', { key: 'ArrowUp' });
  assert.equal(h.document.activeElement, links.at(-1));
  h.emit(links.at(-1), 'keydown', { key: 'ArrowDown' });
  assert.equal(h.document.activeElement, links[0]);
  h.emit(links[0], 'keydown', { key: 'End' });
  assert.equal(h.document.activeElement, links.at(-1));
  h.emit(links.at(-1), 'keydown', { key: 'Home' });
  assert.equal(h.document.activeElement, links[0]);
  trigger.focus();
  h.emit(trigger, 'keydown', { key: 'ArrowUp' });
  assert.equal(h.document.activeElement, links.at(-1));
  h.cleanup();
});

test('Escape closes a dropdown and returns focus to its own trigger', () => {
  const h = createHarness();
  const dropdown = h.dropdowns[1];
  const link = dropdown.querySelector('.dropdown-frame a');
  h.openDropdown(dropdown);
  link.focus();
  const event = h.emit(link, 'keydown', { key: 'Escape' });
  assert.equal(dropdown.open, false);
  assert.equal(event.defaultPrevented, true);
  assert.equal(h.document.activeElement, dropdown.querySelector('summary'));
  h.cleanup();
});

test('Tab remains native; a dropdown stays open for internal focus and closes after focus leaves', () => {
  const h = createHarness();
  const dropdown = h.dropdowns[0];
  const links = [...dropdown.querySelectorAll('.dropdown-frame a')];
  h.openDropdown(dropdown);
  links[0].focus();
  assert.equal(h.emit(links[0], 'keydown', { key: 'Tab' }).defaultPrevented, false);
  h.emit(links[0], 'focusout');
  links[1].focus();
  h.flushFrames();
  assert.equal(dropdown.open, true);
  h.emit(links[1], 'focusout');
  h.toggle.focus();
  h.flushFrames();
  assert.equal(dropdown.open, false);
  h.cleanup();
});

test('dropdown links and outside clicks dismiss without blocking link default behavior', () => {
  const h = createHarness();
  for (const dropdown of h.dropdowns) {
    h.openDropdown(dropdown);
    const link = dropdown.querySelector('.dropdown-frame a');
    const event = h.emit(link.querySelector('svg'), 'click');
    assert.equal(dropdown.open, false, 'Clicking an icon inside a link closes its disclosure');
    assert.equal(event.defaultPrevented, false, 'The browser can follow the link');
    h.openDropdown(dropdown);
    h.emit(h.document.querySelector('main'), 'click');
    assert.equal(dropdown.open, false);
  }
  h.cleanup();
});

test('mobile menu synchronizes visibility and accessible state over repeated clicks', () => {
  const h = createHarness();
  h.openDropdown(h.dropdowns[0]);
  for (let repetition = 0; repetition < 3; repetition += 1) {
    h.emit(h.toggle, 'click');
    assert.equal(h.mobile.open, true);
    assert.equal(h.toggle.getAttribute('aria-expanded'), 'true');
    assert.equal(h.toggle.getAttribute('aria-label'), 'Ryhze menu');
    assert.ok(
      h.dropdowns.filter((dropdown) => dropdown !== h.mobile).every((dropdown) => !dropdown.open),
    );
    h.emit(h.toggle, 'click');
    assert.equal(h.mobile.open, false);
    assert.equal(h.toggle.getAttribute('aria-expanded'), 'false');
    assert.equal(h.toggle.getAttribute('aria-label'), 'Ryhze menu');
  }
  h.cleanup();
});

test('mobile menu dismisses with Escape, navigation links, or outside click', () => {
  const h = createHarness();
  h.emit(h.toggle, 'click');
  h.mobile.querySelector('a').focus();
  h.emit(h.mobile.querySelector('a'), 'keydown', { key: 'Escape' });
  assert.equal(h.mobile.open, false);
  assert.equal(h.toggle.getAttribute('aria-expanded'), 'false');
  assert.equal(h.document.activeElement, h.toggle);
  h.emit(h.toggle, 'click');
  assert.equal(h.emit(h.mobile.querySelector('a'), 'click').defaultPrevented, false);
  assert.equal(h.mobile.open, false);
  h.emit(h.toggle, 'click');
  h.emit(h.document.querySelector('main'), 'click');
  assert.equal(h.mobile.open, false);
  h.cleanup();
});

test('viewport changes and history restoration reset stale open navigation', () => {
  const h = createHarness();
  for (const [target, event] of [
    [h.media, 'change'],
    [h.windowEvents, 'pageshow'],
  ]) {
    h.emit(h.toggle, 'click');
    h.emit(target, event);
    assert.equal(h.mobile.open, false);
    assert.equal(h.toggle.getAttribute('aria-expanded'), 'false');
    h.openDropdown(h.dropdowns[0]);
    h.emit(target, event);
    assert.ok(h.dropdowns.every((dropdown) => !dropdown.open));
  }
  h.cleanup();
});

test('compact menu supports keyboard entry and dismisses after focus leaves', () => {
  const h = createHarness();
  const links = [...h.mobile.querySelectorAll('.dropdown-frame a')];
  h.toggle.focus();
  assert.equal(h.emit(h.toggle, 'keydown', { key: 'ArrowDown' }).defaultPrevented, true);
  assert.equal(h.mobile.open, true);
  assert.equal(h.document.activeElement, links[0]);
  h.emit(links[0], 'keydown', { key: 'End' });
  assert.equal(h.document.activeElement, links.at(-1));
  assert.equal(h.emit(links.at(-1), 'keydown', { key: 'Tab' }).defaultPrevented, false);
  h.emit(links.at(-1), 'focusout');
  h.document.querySelector('main').focus();
  h.flushFrames();
  assert.equal(h.mobile.open, false);
  assert.equal(h.toggle.getAttribute('aria-expanded'), 'false');
  h.cleanup();
});

test('cleanup removes listeners and reinitialization does not double-toggle controls', () => {
  const h = createHarness();
  assert.ok(h.listenerCount() > 0);
  h.cleanup();
  assert.equal(h.listenerCount(), 0);
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, false, 'An unmounted controller no longer responds');
  const cleanup = initializeNavigation(h.document, h.win);
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, true, 'Exactly one active click handler remains');
  cleanup();
  assert.equal(h.listenerCount(), 0);
});

test('summary activation animates entry and exit while closed panels immediately leave the tab order', () => {
  const h = createHarness(undefined, { animate: true });
  const dropdown = h.dropdowns[0];
  const trigger = dropdown.querySelector('summary');
  const frame = dropdown.querySelector('.dropdown-frame');
  trigger.focus();
  assert.equal(h.emit(trigger, 'click').defaultPrevented, true);
  assert.equal(dropdown.open, true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'true');
  assert.equal(frame.hasAttribute('inert'), false);
  assert.equal(h.animations.length, 1);
  assert.equal(h.animations[0].options.duration, 360);
  assert.equal(h.animations[0].keyframes[0].opacity, '0');
  h.animations[0].finish();
  frame.querySelector('a').focus();
  h.emit(frame.querySelector('a'), 'keydown', { key: 'Escape' });
  assert.equal(dropdown.open, true, 'Native panel stays painted during exit');
  assert.equal(dropdown.hasAttribute('data-closing'), true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'false');
  assert.equal(frame.hasAttribute('inert'), true, 'Exiting links cannot receive focus');
  assert.equal(frame.getAttribute('aria-hidden'), 'true');
  assert.equal(h.document.activeElement, trigger);
  h.animations[1].finish();
  assert.equal(dropdown.open, false);
  assert.equal(dropdown.hasAttribute('data-closing'), false);
  h.cleanup();
});

test('rapid open-close-open preserves the painted position and ignores stale completions', () => {
  const h = createHarness(undefined, { animate: true });
  const dropdown = h.dropdowns[0];
  const trigger = dropdown.querySelector('summary');
  h.emit(trigger, 'click');
  const first = h.animations[0];
  const staleOpenFinish = first.onfinish;
  h.emit(trigger, 'click');
  const second = h.animations[1];
  const staleCloseFinish = second.onfinish;
  assert.equal(first.cancelled, true);
  assert.equal(second.keyframes[0].opacity, '0.6');
  assert.equal(second.keyframes[0].transform, 'matrix(1, 0, 0, 1, 0, -3)');
  h.emit(dropdown, 'toggle');
  assert.equal(
    trigger.getAttribute('aria-expanded'),
    'false',
    'Native queued toggle cannot reopen a closing panel',
  );
  h.emit(trigger, 'click');
  assert.equal(second.cancelled, true);
  assert.equal(dropdown.querySelector('.dropdown-frame').hasAttribute('inert'), false);
  staleOpenFinish();
  staleCloseFinish();
  assert.equal(dropdown.open, true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'true');
  h.animations[2].finish();
  assert.equal(dropdown.open, true);
  h.cleanup();
});

test('only the latest opened dropdown remains interactive during cross-panel fades', () => {
  const h = createHarness(undefined, { animate: true });
  const first = h.dropdowns[0];
  const second = h.dropdowns[1];
  h.emit(first.querySelector('summary'), 'click');
  h.emit(second.querySelector('summary'), 'click');
  assert.equal(first.querySelector('summary').getAttribute('aria-expanded'), 'false');
  assert.equal(first.querySelector('.dropdown-frame').hasAttribute('inert'), true);
  assert.equal(second.querySelector('summary').getAttribute('aria-expanded'), 'true');
  assert.equal(second.querySelector('.dropdown-frame').hasAttribute('inert'), false);
  h.cleanup();
});

test('reduced motion opens and closes immediately and settles running animations when enabled', () => {
  const h = createHarness(undefined, { animate: true, reduced: true });
  const dropdown = h.dropdowns[0];
  const trigger = dropdown.querySelector('summary');
  h.emit(trigger, 'click');
  assert.equal(dropdown.open, true);
  h.emit(trigger, 'click');
  assert.equal(dropdown.open, false);
  assert.equal(h.animations.length, 0);
  h.reducedMotion.matches = false;
  h.emit(trigger, 'click');
  h.emit(trigger, 'click');
  assert.equal(dropdown.open, true);
  h.reducedMotion.matches = true;
  h.emit(h.reducedMotion, 'change');
  assert.equal(dropdown.open, false);
  assert.ok(h.animations.every((animation) => animation.cancelled));
  h.cleanup();
});

test('reset and cleanup cancel animation work and prevent queued focus callbacks from mutating UI', () => {
  const h = createHarness(undefined, { animate: true });
  const dropdown = h.dropdowns[0];
  const trigger = dropdown.querySelector('summary');
  h.emit(trigger, 'click');
  h.emit(h.windowEvents, 'pageshow');
  assert.equal(dropdown.open, false);
  assert.equal(h.animations[0].cancelled, true);
  h.emit(trigger, 'click');
  h.emit(trigger, 'focusout');
  h.cleanup();
  h.flushFrames();
  assert.equal(dropdown.open, true, 'Cleanup restores a normal native disclosure');
  assert.equal(trigger.hasAttribute('aria-expanded'), false);
  assert.equal(dropdown.hasAttribute('data-enhanced'), false);
  assert.equal(dropdown.querySelector('.dropdown-frame').hasAttribute('inert'), false);
  assert.ok(h.animations.every((animation) => animation.cancelled));
  assert.equal(h.listenerCount(), 0);
});

test('mobile navigation inherits compact-menu animation, cancellation and reduced motion', () => {
  const h = createHarness(undefined, { animate: true });
  const frame = h.mobile.querySelector('.dropdown-frame');
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, true);
  assert.equal(h.animations.length, 1);
  assert.equal(h.animations[0].options.duration, 360);
  h.animations[0].finish();
  h.emit(h.document.querySelector('main'), 'click');
  assert.equal(frame.hasAttribute('inert'), true);
  assert.equal(h.toggle.getAttribute('aria-expanded'), 'false');
  h.emit(h.toggle, 'click');
  assert.equal(frame.hasAttribute('inert'), false);
  assert.equal(h.toggle.getAttribute('aria-expanded'), 'true');
  h.reducedMotion.matches = true;
  h.emit(h.reducedMotion, 'change');
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, false);
  h.cleanup();
});

test('Entertainment search focuses its field, preserves editing keys and returns focus on Escape', () => {
  const h = createHarness(
    readFileSync(new URL('../dist/games/index.html', import.meta.url), 'utf8'),
  );
  const search = h.document.querySelector('[data-search]');
  const input = search.querySelector('input');
  const trigger = search.querySelector('summary');
  h.emit(trigger, 'click');
  assert.equal(search.open, true);
  assert.equal(h.document.activeElement, input);
  for (const key of ['Home', 'End', 'ArrowUp', 'ArrowDown'])
    assert.equal(h.emit(input, 'keydown', { key }).defaultPrevented, false);
  h.emit(input, 'keydown', { key: 'Escape' });
  assert.equal(search.open, false);
  assert.equal(h.document.activeElement, trigger);
  h.cleanup();
});

test('Entertainment search and menu are mutually exclusive and dismiss through Close, outside and history', () => {
  const h = createHarness(
    readFileSync(new URL('../dist/games/index.html', import.meta.url), 'utf8'),
  );
  const search = h.document.querySelector('[data-search]');
  const menu = h.document.querySelector('.entertainment-menu');
  h.emit(search.querySelector('summary'), 'click');
  h.emit(menu.querySelector('summary'), 'click');
  assert.equal(search.open, false);
  assert.equal(menu.open, true);
  h.emit(h.document.querySelector('h1'), 'click');
  assert.equal(menu.open, false);
  h.emit(search.querySelector('summary'), 'click');
  h.emit(search.querySelector('[data-close-dropdown]'), 'click');
  assert.equal(search.open, false);
  assert.equal(h.document.activeElement, search.querySelector('summary'));
  h.emit(menu.querySelector('summary'), 'click');
  h.emit(h.windowEvents, 'popstate');
  assert.equal(menu.open, false);
  h.cleanup();
});

test('Entertainment panels ignore a stale interrupted closing animation', () => {
  const h = createHarness(
    readFileSync(new URL('../dist/games/index.html', import.meta.url), 'utf8'),
    { animate: true },
  );
  const search = h.document.querySelector('[data-search]');
  const trigger = search.querySelector('summary');
  h.emit(trigger, 'click');
  h.emit(trigger, 'click');
  const stale = h.animations.at(-1).onfinish;
  h.emit(trigger, 'click');
  stale?.();
  assert.equal(search.open, true);
  assert.equal(trigger.getAttribute('aria-expanded'), 'true');
  assert.equal(search.querySelector('.dropdown-frame').hasAttribute('inert'), false);
  h.animations.at(-1).finish();
  h.cleanup();
});

test('all menu and search frames consume shared entry, exit and easing tokens', () => {
  for (const [page, selector] of [
    ['index', '.platform-dropdown'],
    ['index', '.mobile-navigation'],
    ['games', '[data-search]'],
    ['games', '.entertainment-menu'],
  ]) {
    const path = page === 'index' ? 'dist/index.html' : `dist/${page}/index.html`;
    const h = createHarness(readFileSync(new URL(`../${path}`, import.meta.url), 'utf8'), {
      animate: true,
      tokens: {
        '--duration-frame-enter': '.625s',
        '--duration-frame-exit': '175ms',
        '--ease-frame': 'ease-in-out',
      },
    });
    const trigger = h.document.querySelector(`${selector} > summary`);
    h.emit(trigger, 'click');
    assert.equal(h.animations[0].options.duration, 625, selector);
    assert.equal(h.animations[0].options.easing, 'ease-in-out', selector);
    h.animations[0].finish();
    h.emit(trigger, 'click');
    assert.equal(h.animations[1].options.duration, 175, selector);
    assert.equal(h.animations[1].options.easing, 'ease-in-out', selector);
    h.animations[1].finish();
    h.cleanup();
  }
});

test('explicit zero frame durations settle immediately without hidden animation work', () => {
  const h = createHarness(undefined, {
    animate: true,
    tokens: { '--duration-frame-enter': '0s', '--duration-frame-exit': '0ms' },
  });
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, true);
  h.emit(h.toggle, 'click');
  assert.equal(h.mobile.open, false);
  assert.equal(h.animations.length, 0);
  h.cleanup();
});
