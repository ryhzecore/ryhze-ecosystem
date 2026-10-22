import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import { cascade } from './helpers/css-cascade.mjs';

const document = (name) =>
  parseHTML(readFileSync(new URL(`../dist/${name}/index.html`, import.meta.url), 'utf8')).document;
const text = (element) => element.textContent.replace(/\s+/g, ' ').trim();
const services = [
  'Foundation',
  'Platform Abstraction',
  'Memory',
  'Jobs',
  'Timing',
  'Configuration',
  'Diagnostics',
  'Runtime Lifecycle',
];
const runtime = [
  'Scene & World',
  'Assets & Resources',
  'Graphics & Rendering',
  'Animation & Cinematics',
  'Physics & Simulation',
  'Audio',
  'Gameplay Framework',
  'Scripting & Visual Scripting',
  'Networking & Multiplayer',
  'UI & Input',
];
const expectedStages = {
  services,
  runtime,
  authoring: ['Editor & Authoring', 'Film/Mocap/VFX tools'],
  packaging: ['Platform/Packaging/Export'],
  outputs: ['Games', 'Films/Cinematics'],
};

function stage(figure, name) {
  const result = figure.querySelector(`.flow-stage[data-stage="${name}"]`);
  assert.ok(result, `${name} has a native stage`);
  return result;
}
function names(element) {
  return [...element.querySelectorAll('.flow-name')].map(text);
}

test('architecture has a complete readable native graph, current site shell and return navigation', () => {
  const doc = document('race/architecture');
  assert.equal(doc.querySelectorAll('h1').length, 1);
  assert.equal(text(doc.querySelector('h1')), 'Architecture');
  assert.ok(doc.querySelector('header a[href="/race/"][aria-current="page"]'));
  assert.ok(doc.querySelector('main a.button[href="/race/"]'));
  const figure = doc.querySelector('figure.architecture-flow[aria-label]');
  assert.ok(figure);
  assert.ok(figure.querySelector('figcaption'));
  assert.equal(
    figure.querySelectorAll('img, picture, canvas, iframe, object, embed, script').length,
    0,
  );
  assert.equal(figure.querySelectorAll('[aria-hidden="true"] .flow-name').length, 0);
  assert.equal(figure.querySelectorAll('.flow-node').length, 28);
  assert.ok(figure.querySelectorAll('ul.flow-rail > li.flow-node > .flow-name').length > 0);
  for (const [name, expected] of Object.entries(expectedStages)) {
    const group = stage(figure, name);
    if (name !== 'services') assert.ok(group.querySelector('.flow-hub > h2'));
    assert.deepEqual(names(group), expected, name);
  }
});

test('Core service cards connect directly below Core without a duplicate visible group label', () => {
  for (const name of ['race', 'race/architecture']) {
    const figure = document(name).querySelector('figure.architecture-flow');
    assert.doesNotMatch(text(figure), /core services/i);
    assert.equal(
      text(figure.querySelector('[data-node="core"] h2, [data-node="core"] h3')),
      'Core',
    );
  }
  const group = stage(
    document('race/architecture').querySelector('.architecture-flow'),
    'services',
  );
  assert.equal(group.getAttribute('aria-label'), 'Core services');
  assert.equal(group.querySelector('h2, h3, .flow-label'), null);
  assert.ok(group.firstElementChild.classList.contains('flow-columns'));
  assert.deepEqual(names(group), services);
});

test('eight Core services belong to the implemented hub while every future node has the exact status', () => {
  const doc = document('race/architecture');
  const figure = doc.querySelector('.architecture-flow');
  const hub = figure.querySelector('.flow-hub');
  assert.equal(text(hub.querySelector('h2')), 'Core');
  assert.match(text(hub), /implemented and tested on Linux/i);
  assert.deepEqual(names(stage(figure, 'services')), services);
  const nodes = [...figure.querySelectorAll('.flow-rail > .flow-node')];
  const byName = new Map(nodes.map((node) => [text(node.querySelector('.flow-name')), node]));
  assert.equal(byName.size, 23);
  for (const name of services) {
    const node = byName.get(name);
    assert.ok(node.classList.contains('flow-node--current'), name);
    assert.equal(node.querySelector('.flow-status'), null, name);
  }
  const scene = byName.get('Scene & World');
  assert.equal(
    text(scene.querySelector('.flow-status')),
    'First Linux headless milestone signed off',
  );
  // Preserve the existing visual treatment: this legacy class is not an acceptance state.
  assert.ok(scene.classList.contains('flow-node--pending'));
  assert.ok(!scene.classList.contains('flow-node--current'));
  const assets = byName.get('Assets & Resources');
  assert.equal(
    text(assets.querySelector('.flow-status')),
    'First Linux headless milestone signed off',
  );
  assert.ok(assets.classList.contains('flow-node--pending'));
  assert.ok(!assets.classList.contains('flow-node--current'));
  const future = nodes.filter(
    (node) =>
      !services.includes(text(node.querySelector('.flow-name'))) &&
      node !== scene &&
      node !== assets,
  );
  assert.equal(future.length, 13);
  for (const node of future) {
    assert.equal(text(node.querySelector('.flow-status')), 'Under Development');
    assert.ok(!node.classList.contains('flow-node--current'));
    assert.ok(!node.classList.contains('flow-node--pending'));
    const style = cascade(node, 1440);
    assert.equal(style.background, '#181818');
    assert.equal(style.border, '1px solid #323232');
  }
});

test('runtime nodes share the Core foundation rather than nesting below Scene or their neighbours', () => {
  for (const name of ['race', 'race/architecture']) {
    const figure = document(name).querySelector('figure.architecture-flow');
    assert.ok(figure, name);
    const root = figure.querySelector('.flow-root');
    assert.equal(figure.querySelectorAll('.flow-root').length, 1);
    assert.equal(root.firstElementChild, figure.querySelector('.flow-hub'));
    const stages = [...root.children].filter((element) => element.classList.contains('flow-stage'));
    assert.deepEqual(
      stages.map((element) => element.getAttribute('data-stage')),
      name === 'race'
        ? ['runtime', 'authoring', 'packaging', 'outputs']
        : Object.keys(expectedStages),
    );
    const runtimeStage = stage(figure, 'runtime');
    for (const node of runtimeStage.querySelectorAll('.flow-rail > li.flow-node')) {
      assert.equal(node.parentElement.localName, 'ul');
      assert.ok(node.parentElement.classList.contains('flow-rail'));
      assert.equal(node.closest('[data-stage]'), runtimeStage);
      assert.equal(node.querySelectorAll('.flow-node, .flow-stage, .flow-rail').length, 0);
    }
    assert.equal(figure.querySelectorAll('.flow-node .flow-node').length, 0);
    const explanation = text(figure.querySelector('figcaption'));
    assert.match(explanation, /conceptual connections/i);
    assert.match(explanation, /not an approved dependency graph/i);
    assert.match(explanation, /each runtime area branches from the shared Core foundation/i);
    assert.match(explanation, /not from Scene & World or a neighbouring area/i);
  }
});

test('Runtime & Modules is a shared-foundation parent with the same frame and heading treatment as Core', () => {
  for (const name of ['race', 'race/architecture']) {
    const figure = document(name).querySelector('figure.architecture-flow');
    const core = figure.querySelector('[data-node="core"]');
    const runtimeStage = stage(figure, 'runtime');
    const parent = runtimeStage.querySelector('[data-node="runtime"]');
    assert.ok(parent, `${name} has the Runtime & Modules parent`);
    assert.equal(runtimeStage.firstElementChild, parent);
    const columns = parent.nextElementSibling;
    assert.ok(columns.classList.contains('flow-columns'));
    assert.equal(columns.parentElement, runtimeStage);
    const headingTag = name === 'race' ? 'h3' : 'h2';
    const coreHeading = core.querySelector(headingTag);
    const runtimeHeading = parent.querySelector(headingTag);
    assert.ok(coreHeading);
    assert.ok(runtimeHeading);
    assert.equal(text(runtimeHeading), 'Runtime & Modules');
    assert.equal(text(parent.querySelector('.flow-status')), 'Shared Core foundation');
    assert.doesNotMatch(text(parent), /implemented|tested|signed off|complete|production.ready/i);
    assert.deepEqual(
      names(columns),
      name === 'race' ? ['Scene & World', 'Future runtime areas'] : runtime,
    );
    assert.equal(parent.querySelectorAll('.flow-node, .flow-rail').length, 0);
    for (const viewport of [320, 390, 760, 768, 1440]) {
      const frame = cascade(parent, viewport);
      const coreFrame = cascade(core, viewport);
      for (const property of [
        'border-radius',
        'border',
        'border-color',
        'background',
        'box-shadow',
        'padding-top',
        'padding-right',
        'padding-bottom',
        'padding-left',
      ]) {
        assert.notEqual(frame[property], undefined, `${name} ${viewport} ${property}`);
        assert.equal(frame[property], coreFrame[property], `${name} ${viewport} ${property}`);
      }
      assert.equal(frame['border-radius'], '32px');
      assert.equal(frame['border-color'], '#ffffff');
      assert.equal(frame.background, '#111111');
      assert.equal(frame['padding-top'], viewport <= 760 ? '16px' : '24px');
      const typography = cascade(runtimeHeading, viewport);
      const coreTypography = cascade(coreHeading, viewport);
      for (const property of ['font-family', 'font-weight', 'letter-spacing', 'line-height']) {
        assert.notEqual(typography[property], undefined, `${name} ${viewport} ${property}`);
        assert.equal(
          typography[property],
          coreTypography[property],
          `${name} ${viewport} ${property}`,
        );
      }
      assert.equal(
        typography['font-size'],
        name === 'race/architecture' && viewport <= 760 ? '1.8rem' : coreTypography['font-size'],
      );
      assert.equal(cascade(columns, viewport)['margin-top'], '32px');
    }
  }
});

test('the three later parent groups reuse neutral hub frames and retain their exact children', () => {
  const titles = {
    authoring: 'Later authoring',
    packaging: 'Platform, packaging and export',
    outputs: 'Project outputs',
  };
  for (const name of ['race', 'race/architecture']) {
    const figure = document(name).querySelector('.architecture-flow');
    const headingTag = name === 'race' ? 'h3' : 'h2';
    const runtimeParent = figure.querySelector('[data-node="runtime"]');
    for (const [id, title] of Object.entries(titles)) {
      const group = stage(figure, id);
      const parent = group.firstElementChild;
      assert.equal(group.getAttribute('aria-label'), title);
      assert.equal(parent.getAttribute('data-node'), id);
      assert.ok(parent.classList.contains('flow-hub'));
      assert.ok(parent.classList.contains('flow-node'));
      assert.ok(!parent.classList.contains('flow-node--current'));
      assert.ok(!parent.classList.contains('flow-node--pending'));
      assert.equal(text(parent.querySelector(headingTag)), title);
      assert.equal(text(parent.querySelector('.flow-status')), 'Under Development');
      assert.equal(parent.querySelectorAll('.flow-node, .flow-rail').length, 0);
      const columns = parent.nextElementSibling;
      assert.ok(columns.classList.contains('flow-columns'));
      assert.equal(columns.parentElement, group);
      assert.deepEqual(names(columns), expectedStages[id]);
      for (const viewport of [320, 390, 760, 768, 1440]) {
        const frame = cascade(parent, viewport);
        const reference = cascade(runtimeParent, viewport);
        assert.equal(frame.border, '1px solid #323232');
        assert.equal(frame.background, '#181818');
        for (const property of [
          'border-radius',
          'box-shadow',
          'min-width',
          'padding-top',
          'padding-right',
          'padding-bottom',
          'padding-left',
        ])
          assert.equal(frame[property], reference[property], `${id} ${viewport} ${property}`);
        const typography = cascade(parent.querySelector(headingTag), viewport);
        const referenceTypography = cascade(runtimeParent.querySelector(headingTag), viewport);
        assert.equal(
          typography['font-size'],
          ['authoring', 'packaging'].includes(id) && viewport <= 760
            ? '1.5rem'
            : referenceTypography['font-size'],
        );
        for (const property of ['font-family', 'font-weight', 'letter-spacing', 'line-height'])
          assert.equal(
            typography[property],
            referenceTypography[property],
            `${id} ${viewport} ${property}`,
          );
        assert.equal(cascade(columns, viewport)['margin-top'], '32px');
        assert.notEqual(frame.overflow, 'hidden');
      }
    }
    assert.equal(figure.querySelectorAll('.flow-label').length, 0);
  }
});

test('landing contains a useful connected compact overview and the full page expands its runtime areas', () => {
  const compact = document('race').querySelector('.architecture-flow--compact');
  assert.ok(compact);
  assert.equal(compact.querySelectorAll('.flow-node').length, 12);
  assert.match(text(compact.querySelector('.flow-hub')), /eight shared services/i);
  assert.deepEqual(names(stage(compact, 'runtime')), ['Scene & World', 'Future runtime areas']);
  for (const name of ['authoring', 'packaging', 'outputs'])
    assert.deepEqual(names(stage(compact, name)), expectedStages[name]);
  const expanded = document('race/architecture').querySelector('.architecture-flow');
  assert.ok(!expanded.classList.contains('architecture-flow--compact'));
  assert.deepEqual(names(stage(expanded, 'runtime')), runtime);
  for (const node of compact.querySelectorAll('.flow-rail > .flow-node:not(.flow-node--pending)'))
    assert.equal(text(node.querySelector('.flow-status')), 'Under Development');
});

test('architecture reflows native nodes into one column and keeps the shared Ryhze surfaces', () => {
  for (const name of ['race', 'race/architecture']) {
    const doc = document(name);
    const root = doc.querySelector('.flow-root');
    for (const viewport of [320, 390, 700, 760, 768, 960, 1440]) {
      assert.equal(cascade(root, viewport)['border-left'], '2px dashed #ffffff');
      for (const columns of doc.querySelectorAll('.flow-columns')) {
        const style = cascade(columns, viewport);
        assert.equal(style.display, 'grid');
        assert.equal(
          style['grid-template-columns'],
          viewport <= 760 || columns.classList.contains('flow-columns--single')
            ? 'minmax(0,1fr)'
            : 'repeat(2,minmax(0,1fr))',
        );
        // A full-width border would extend past the final branch rail; the
        // bounded connector is anchored to this positioned, unclipped grid.
        assert.equal(style['border-top'], undefined);
        assert.equal(style.position, 'relative');
        assert.notEqual(style.overflow, 'hidden');
      }
      for (const rail of doc.querySelectorAll('.flow-rail'))
        assert.equal(cascade(rail, viewport)['border-left'], '2px dashed #ffffff');
      for (const node of doc.querySelectorAll('.flow-node')) {
        const style = cascade(node, viewport);
        assert.equal(style['border-radius'], '32px');
        assert.equal(style['min-width'], '0');
        assert.notEqual(style.overflow, 'hidden');
      }
      for (const label of doc.querySelectorAll('.flow-name'))
        assert.equal(cascade(label, viewport)['overflow-wrap'], 'anywhere');
    }
  }
  // CSS/DOM assertions are not geometric proof: measured connector endpoints,
  // non-overlap, enlarged text and actual pixels are separate browser gates.
});

test('architecture keeps the signed-off Linux milestone and wider platform limits explicit', () => {
  const doc = document('race/architecture');
  const main = text(doc.querySelector('main'));
  assert.match(main, /only Linux is currently implemented and tested/i);
  assert.match(main, /first Linux headless milestone passed its test gates/i);
  assert.match(main, /signed off after source and evidence review/i);
  assert.match(main, /desktop Windows, Linux and macOS/i);
  assert.match(main, /later game exports target Android and iOS/i);
  assert.match(main, /no public engine download/i);
  assert.doesNotMatch(
    main,
    /To Be Decided|worlds in motion|Taking shape|Small responsibilities|fully complete|production.ready/i,
  );
  assert.doesNotMatch(main, /benchmark|full-trial|jobs per second|\bp95\b|\bp99\b|\bFPS\b|\b8K\b/i);
  assert.equal(doc.querySelectorAll('main a[download], main a[href*="/race/progress"]').length, 0);
  assert.doesNotMatch(
    doc.documentElement.outerHTML,
    /docs\.google\.com|\/workspace\/|Sentinel_|github\.com\/ryhzecore|library-file:|libfile_/i,
  );
});
