import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import { parseHTML } from 'linkedom';
import ts from 'typescript';

const source = readFileSync(new URL('../src/scripts/ambient.ts', import.meta.url), 'utf8');
const compiled = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
}).outputText;
const context = { exports: {} };
vm.runInNewContext(compiled, context, { filename: 'ambient.js' });
const { initializeAmbient } = context.exports;

// Event/animation-frame simulation verifies the actual controller, not browser rendering.
function harness({ reduced = false, scroll = 0, markup = true } = {}) {
  const { document } = parseHTML(
    markup
      ? '<html><body><div data-ambient-background><div data-ambient-glow="top"></div><div data-ambient-glow="bottom"></div></div></body></html>'
      : '<html><body></body></html>',
  );
  const media = new EventTarget();
  media.matches = reduced;
  const events = new EventTarget();
  const frames = new Map();
  let id = 0;
  const win = {
    scrollY: scroll,
    matchMedia(query) {
      assert.equal(query, '(prefers-reduced-motion: reduce)');
      return media;
    },
    addEventListener: events.addEventListener.bind(events),
    removeEventListener: events.removeEventListener.bind(events),
    requestAnimationFrame(callback) {
      frames.set(++id, callback);
      return id;
    },
    cancelAnimationFrame(key) {
      frames.delete(key);
    },
  };
  const cleanup = initializeAmbient(document, win);
  return {
    win,
    media,
    frames,
    cleanup,
    background: document.querySelector('[data-ambient-background]'),
    glows: [...document.querySelectorAll('[data-ambient-glow]')],
    emit(name) {
      events.dispatchEvent(new Event(name));
    },
    flush() {
      const callbacks = [...frames.values()];
      frames.clear();
      callbacks.forEach((callback) => callback());
    },
    preference(value) {
      media.matches = value;
      media.dispatchEvent(new Event('change'));
    },
  };
}

test('ambient glows start correctly at a restored scroll position and move upward at gentle rates', () => {
  const h = harness({ scroll: 1000 });
  assert.equal(h.frames.size, 1);
  h.flush();
  assert.equal(h.glows[0].style.transform, 'translate3d(0, -75px, 0)');
  assert.equal(h.glows[1].style.transform, 'translate3d(0, -110px, 0)');
  h.win.scrollY = 500;
  h.emit('scroll');
  h.flush();
  assert.equal(h.glows[0].style.transform, 'translate3d(0, -37.5px, 0)');
  assert.equal(h.glows[1].style.transform, 'translate3d(0, -55px, 0)');
  h.cleanup();
});

test('repeated scroll events coalesce to one frame and never start an autonomous loop', () => {
  const h = harness();
  h.flush();
  for (let i = 0; i < 50; i++) {
    h.win.scrollY = i * 10;
    h.emit('scroll');
  }
  assert.equal(h.frames.size, 1);
  h.flush();
  assert.equal(h.glows[0].style.transform, 'translate3d(0, -36.75px, 0)');
  assert.equal(h.frames.size, 0);
  h.cleanup();
});

test('reduced-motion users get stationary glows with no scheduled animation frames', () => {
  const h = harness({ reduced: true, scroll: 500 });
  h.emit('scroll');
  h.emit('pageshow');
  assert.equal(h.frames.size, 0);
  assert.ok(h.glows.every((glow) => !glow.style.transform));
  h.cleanup();
});

test('changing reduced-motion preference cancels pending frames, resets styles and can resume', () => {
  const h = harness({ scroll: 500 });
  h.flush();
  h.emit('scroll');
  h.preference(true);
  assert.equal(h.frames.size, 0);
  assert.ok(h.glows.every((glow) => !glow.style.transform));
  h.preference(false);
  assert.equal(h.frames.size, 1);
  h.flush();
  assert.equal(h.glows[1].style.transform, 'translate3d(0, -55px, 0)');
  h.cleanup();
});

test('page restoration resynchronizes scroll; overscroll and very long pages are bounded', () => {
  const h = harness();
  h.flush();
  h.win.scrollY = -40;
  h.emit('scroll');
  h.flush();
  assert.ok(h.glows.every((glow) => glow.style.transform === 'translate3d(0, 0px, 0)'));
  h.win.scrollY = 100000;
  h.emit('pageshow');
  h.flush();
  assert.ok(h.glows.every((glow) => glow.style.transform === 'translate3d(0, -240px, 0)'));
  h.cleanup();
});

test('scroll and motion preference changes never mutate the static bottom-edge background', () => {
  const h = harness();
  for (const scroll of [0, 1000, 100000, 0]) {
    h.win.scrollY = scroll;
    h.emit('pageshow');
    h.emit('scroll');
    h.flush();
    assert.equal(h.background.getAttribute('style'), null);
  }
  h.preference(true);
  h.preference(false);
  h.flush();
  assert.equal(h.background.getAttribute('style'), null);
  h.cleanup();
  assert.equal(h.background.getAttribute('style'), null);
});

test('cleanup removes pending work and listeners; an absent glow layer is a safe no-op', () => {
  const h = harness();
  const stale = [...h.frames.values()][0];
  h.cleanup();
  h.emit('scroll');
  h.emit('pageshow');
  h.preference(false);
  stale();
  assert.equal(h.frames.size, 0);
  assert.ok(h.glows.every((glow) => !glow.style.transform));
  const empty = harness({ markup: false });
  assert.equal(empty.frames.size, 0);
  empty.cleanup();
});
