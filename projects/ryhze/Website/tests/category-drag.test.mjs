import assert from 'node:assert/strict';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { scriptModule } from './helpers/script-module.mjs';
const { initializeCategoryDrag } = scriptModule('category-drag', { AbortController });
function harness(category = 'games') {
  const { document, window } = parseHTML(
    `<nav data-entertainment-chooser data-category="${category}"><span class="entertainment-selection"></span><a data-category-link="games" href="/games/">Games</a><a data-category-link="films" href="/films/">Films</a></nav>`,
  );
  const chooser = document.querySelector('nav'),
    links = [...chooser.querySelectorAll('a')],
    navigated = [];
  const add = chooser.addEventListener.bind(chooser);
  chooser.addEventListener = (type, callback, opts) => {
    add(type, callback);
    opts?.signal?.addEventListener('abort', () => chooser.removeEventListener(type, callback), {
      once: true,
    });
  };
  links.forEach((link, i) => {
    link.getBoundingClientRect = () => ({ left: i * 84, width: 80 });
    link.click = () => navigated.push(link.getAttribute('href'));
  });
  const emit = (type, x = 40, y = 0, target = chooser, extra = {}) => {
    const e = new window.Event(type, { bubbles: true, cancelable: true });
    Object.assign(e, {
      pointerId: 1,
      clientX: x,
      clientY: y,
      button: 0,
      isPrimary: true,
      ...extra,
    });
    target.dispatchEvent(e);
    return e;
  };
  const cleanup = initializeCategoryDrag(document);
  return { chooser, links, navigated, emit, cleanup };
}
for (const category of ['games', 'films'])
  test(`drag selects the other native link from ${category}`, () => {
    const h = harness(category),
      from = category === 'games' ? 40 : 124,
      to = category === 'games' ? 124 : 40;
    h.emit('pointerdown', from);
    h.emit('pointermove', (from + to) / 2);
    assert.equal(h.navigated.length, 0);
    assert.ok(h.chooser.querySelector('span').style.transform.includes('42px'));
    h.emit('pointermove', to);
    h.emit('pointerup', to);
    assert.deepEqual(h.navigated, [category === 'games' ? '/films/' : '/games/']);
    assert.equal(h.emit('click', to, 0, h.links[0], { detail: 1 }).defaultPrevented, true);
    assert.equal(h.emit('click', to, 0, h.links[0], { detail: 0 }).defaultPrevented, false);
  });
test('short, vertical, cancelled and lost-capture drags do not navigate', () => {
  for (const kind of ['short', 'vertical', 'pointercancel', 'lostpointercapture']) {
    const h = harness();
    h.emit('pointerdown', 40);
    if (kind === 'short') {
      h.emit('pointermove', 44);
      h.emit('pointerup', 44);
    } else if (kind === 'vertical') {
      h.emit('pointermove', 45, 40);
      h.emit('pointerup', 124, 40);
    } else {
      h.emit('pointermove', 124);
      h.emit(kind, 124);
    }
    assert.deepEqual(h.navigated, []);
    assert.equal(h.chooser.hasAttribute('data-dragging'), false);
  }
});
test('drag snaps back, allows subsequent taps and resets on cleanup', () => {
  const h = harness();
  h.emit('pointerdown', 40);
  h.emit('pointermove', 60);
  h.emit('pointerup', 60);
  assert.deepEqual(h.navigated, []);
  h.emit('pointerdown', 124);
  h.emit('pointerup', 124);
  assert.equal(h.emit('click', 124, 0, h.links[1], { detail: 1 }).defaultPrevented, false);
  h.emit('pointerdown', 40);
  h.emit('pointermove', 70);
  h.cleanup();
  assert.equal(h.chooser.hasAttribute('data-dragging'), false);
});
