import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setMaxListeners } from 'node:events';
import vm from 'node:vm';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import ts from 'typescript';

const source = readFileSync(new URL('../src/scripts/skip-link.ts', import.meta.url), 'utf8');
const context = { exports: {}, AbortController };
vm.runInNewContext(
  ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  }).outputText,
  context,
);

// Production controller against built HTML. Focus/event primitives are simulated;
// these tests cannot establish browser-native focus, touch hardware or rendering.
function harness(html = readFileSync(new URL('../dist/index.html', import.meta.url), 'utf8')) {
  const { document, window } = parseHTML(html);
  const windowEvents = new window.EventTarget();
  let active = document.body;
  Object.defineProperty(document, 'activeElement', { get: () => active });
  let listeners = 0;
  for (const target of [document, windowEvents, ...document.querySelectorAll('*')]) {
    const add = target.addEventListener.bind(target);
    const remove = target.removeEventListener.bind(target);
    target.addEventListener = (type, fn, options) => {
      if (options?.signal?.aborted) return;
      add(type, fn, options);
      listeners++;
      if (options?.signal) {
        setMaxListeners(0, options.signal);
        options.signal.addEventListener(
          'abort',
          () => {
            remove(type, fn);
            listeners--;
          },
          { once: true },
        );
      }
    };
    if (target.nodeType === 1) {
      target.focus = () => {
        active?.dispatchEvent(new window.Event('blur'));
        active = target;
        target.dispatchEvent(new window.Event('focus'));
        target.dispatchEvent(new window.Event('focusin', { bubbles: true }));
      };
      target.blur = () => {
        if (active === target) active = document.body;
        target.dispatchEvent(new window.Event('blur'));
      };
    }
  }
  const emit = (target, type, props = {}) => {
    const event = new window.Event(type, { bubbles: true, cancelable: true });
    Object.assign(event, props);
    target.dispatchEvent(event);
    return event;
  };
  const win = { addEventListener: windowEvents.addEventListener.bind(windowEvents) };
  const cleanup = context.exports.initializeSkipLink(document, win);
  const link = document.querySelector('.skip-link');
  const tabToLink = () => {
    emit(document, 'keydown', { key: 'Tab' });
    link.focus();
    emit(document, 'keyup', { key: 'Tab' });
  };
  return { document, windowEvents, emit, link, tabToLink, cleanup, listeners: () => listeners };
}

test('skip link stays hidden on initial, programmatic, touch and restored focus', () => {
  const h = harness();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.link.focus();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.emit(h.document, 'touchstart');
  h.link.focus();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.emit(h.windowEvents, 'pageshow');
  assert.notEqual(h.document.activeElement, h.link);
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.cleanup();
});

test('Tab and Shift+Tab reveal the link; blur, pointer and wheel clear it', () => {
  const h = harness();
  for (const reset of ['blur', 'pointerdown', 'mousedown', 'touchstart', 'wheel']) {
    h.tabToLink();
    assert.equal(h.link.hasAttribute('data-keyboard-focus'), true);
    if (reset === 'blur') h.link.blur();
    else h.emit(h.document.body, reset);
    assert.equal(h.link.hasAttribute('data-keyboard-focus'), false, reset);
  }
  h.emit(h.document, 'keydown', { key: 'Tab', shiftKey: true });
  h.link.focus();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), true);
  // Scrolling alone must not steal deliberate keyboard focus.
  h.emit(h.windowEvents, 'scroll');
  assert.equal(h.document.activeElement, h.link);
  h.cleanup();
});

test('skip activation keeps native navigation and transfers focus to the main landmark', () => {
  const h = harness();
  h.tabToLink();
  h.emit(h.link, 'pointerdown');
  assert.equal(
    h.link.hasAttribute('data-keyboard-focus'),
    true,
    'A keyboard-revealed link can still be clicked',
  );
  assert.equal(h.emit(h.link, 'click').defaultPrevented, false);
  assert.equal(h.document.activeElement, h.document.getElementById('main'));
  assert.equal(h.document.activeElement.getAttribute('tabindex'), '-1');
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.cleanup();
});

test('Tab intent does not leak into a later programmatic focus and cleanup removes listeners', () => {
  const h = harness();
  h.emit(h.document, 'keydown', { key: 'Tab' });
  h.document.getElementById('main').focus();
  h.link.focus();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.emit(h.document, 'keyup', { key: 'Tab' });
  h.link.focus();
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.tabToLink();
  h.emit(h.document, 'keydown', { key: 'Escape' });
  assert.equal(h.link.hasAttribute('data-keyboard-focus'), false);
  h.cleanup();
  assert.equal(h.listeners(), 0);
  assert.equal(h.link.hasAttribute('data-skip-enhanced'), false);
  assert.doesNotThrow(harness('<html><body><main></main></body></html>').cleanup);
});

test('pointer-driven modal autofocus and card focus return do not acquire keyboard styling', () => {
  const h = harness(readFileSync(new URL('../dist/games/index.html', import.meta.url), 'utf8'));
  const root = h.document.documentElement;
  const tile = h.document.querySelector('.entertainment-thumbnail');
  const back = h.document.querySelector('[data-close-title]');
  assert.equal(root.hasAttribute('data-pointer'), true, 'Initial programmatic focus is quiet');
  for (const type of ['pointerdown', 'touchstart', 'mousedown']) {
    h.emit(tile, type);
    back.focus();
    assert.equal(h.document.activeElement, back, 'Modal focus itself is preserved');
    assert.equal(root.hasAttribute('data-pointer'), true);
    tile.focus();
    assert.equal(h.document.activeElement, tile, 'Focus return itself is preserved');
    assert.equal(root.hasAttribute('data-pointer'), true);
  }
  h.cleanup();
  assert.equal(root.hasAttribute('data-pointer'), false, 'Cleanup restores native focus styling');
});

test('mixed keyboard and pointer input persists through keyup, focus movement and restored pages', () => {
  const h = harness(readFileSync(new URL('../dist/games/index.html', import.meta.url), 'utf8'));
  const root = h.document.documentElement;
  const tile = h.document.querySelector('.entertainment-thumbnail');
  const back = h.document.querySelector('[data-close-title]');
  for (const key of ['Tab', 'Enter', ' ', 'ArrowDown', 'Escape']) {
    h.emit(tile, 'touchstart');
    h.emit(h.document, 'keydown', { key });
    assert.equal(root.hasAttribute('data-pointer'), false, key);
    h.emit(h.document, 'keyup', { key });
    back.focus();
    tile.focus();
    h.emit(h.windowEvents, 'pageshow', { persisted: true });
    assert.equal(
      root.hasAttribute('data-pointer'),
      false,
      'Keyboard intent survives modal and restoration focus',
    );
    h.emit(back, 'pointerdown');
    assert.equal(root.hasAttribute('data-pointer'), true);
  }
  for (const modifier of ['altKey', 'ctrlKey', 'metaKey']) {
    h.emit(h.document, 'keydown', { key: 'Tab', [modifier]: true });
    assert.equal(
      root.hasAttribute('data-pointer'),
      true,
      'OS/browser shortcuts do not imply app keyboard navigation',
    );
  }
  h.cleanup();
});
