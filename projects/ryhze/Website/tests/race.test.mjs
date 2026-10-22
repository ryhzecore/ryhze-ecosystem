import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import { parseHTML } from 'linkedom';
import { cascade } from './helpers/css-cascade.mjs';

const page = (name) =>
  parseHTML(readFileSync(new URL(`../dist/${name}/index.html`, import.meta.url), 'utf8')).document;
const text = (element) => element.textContent.replace(/\s+/g, ' ').trim();
function section(doc, title) {
  const heading = [...doc.querySelectorAll('main h2')].find((node) => text(node) === title);
  assert.ok(heading, `${title} has a factual section heading`);
  const result = heading.closest('section');
  assert.ok(result, `${title} belongs to a semantic section`);
  return result;
}

test('RACE landing uses factual headings and prominent navigation to the complete architecture', () => {
  const doc = page('race');
  assert.equal(doc.querySelectorAll('h1').length, 1);
  assert.equal(text(doc.querySelector('h1')), 'RACE Engine');
  assert.ok(doc.querySelector('header a[href="/race/"][aria-current="page"]'));
  assert.ok(doc.querySelector('footer a[href="/race/"]'));
  for (const title of ['Core', 'Scene & World', 'Architecture', 'Development Status'])
    section(doc, title);
  assert.ok(doc.querySelector('main a.button[href="/race/architecture/"]'));
  assert.ok(section(doc, 'Architecture').querySelector('a[href="/race/architecture/"]'));
  const navigation = doc.querySelector('.race-contents[aria-label]');
  assert.ok(navigation);
  for (const link of doc.querySelectorAll('a[href^="#"]'))
    assert.ok(doc.getElementById(link.getAttribute('href').slice(1)));
  for (const element of doc.querySelectorAll('main section[aria-labelledby]'))
    assert.ok(doc.getElementById(element.getAttribute('aria-labelledby')));
});

test('RACE explains implemented Linux Core and the signed-off first headless Scene milestone', () => {
  const doc = page('race');
  const core = text(section(doc, 'Core'));
  assert.match(core, /Linux/);
  assert.match(core, /implement/i);
  assert.match(core, /test/i);
  for (const term of [
    /memory/i,
    /jobs/i,
    /timing/i,
    /configuration/i,
    /diagnostics/i,
    /lifecycle/i,
  ])
    assert.match(core, term);
  const scene = text(section(doc, 'Scene & World'));
  assert.match(scene, /Linux/);
  assert.match(scene, /headless/i);
  assert.match(scene, /hierarch/i);
  assert.match(scene, /transform/i);
  assert.match(scene, /passed its test gates/i);
  assert.match(scene, /signed off after source and evidence review/i);
  assert.match(scene, /not a graphical editor or a completed engine/i);
  assert.doesNotMatch(scene, /production.ready|fully complete/i);
});

test('Scene sign-off is consistent across landing, architecture, diagram nodes and page metadata', () => {
  const status = 'First Linux headless milestone signed off';
  const summary =
    'The first Linux headless milestone passed its test gates and is signed off after source and evidence review.';
  for (const name of ['race', 'race/architecture']) {
    const doc = page(name);
    const sceneNode = [...doc.querySelectorAll('.flow-node')].find(
      (node) => text(node.querySelector('.flow-name') ?? node) === 'Scene & World',
    );
    assert.ok(sceneNode, `${name} includes the Scene node`);
    assert.equal(text(sceneNode.querySelector('.flow-status')), status, name);
    assert.ok(text(doc.querySelector('main')).includes(summary), name);
    const descriptions = [...doc.querySelectorAll('meta[content]')]
      .map((meta) => meta.getAttribute('content'))
      .join(' ');
    assert.doesNotMatch(
      `${text(doc.body)} ${descriptions}`,
      /acceptance[^.!?]*\b(?:pending|open)\b|awaiting[^.!?]*acceptance|\bin review\b/i,
      `${name} does not retain stale acceptance wording in visible copy or metadata`,
    );
  }
  const doc = page('race');
  const scene = section(doc, 'Scene & World');
  assert.equal(text(scene.querySelector('.eyebrow')), status);
  assert.ok(text(scene).includes(summary));
  assert.ok(
    text(scene).includes('source and evidence review. This is not a graphical editor'),
    'the rendered landing summary keeps the sentence boundary after interpolation',
  );
  assert.ok(
    text(page('race/architecture').querySelector('main')).includes(
      'source and evidence review. Every future area',
    ),
    'the rendered architecture summary keeps the sentence boundary after interpolation',
  );
  const statusTerm = [...doc.querySelectorAll('.race-status-list dt')].find(
    (term) => text(term) === 'Scene & World',
  );
  assert.ok(statusTerm);
  assert.equal(text(statusTerm.nextElementSibling), summary);
});

test('Assets sign-off is factual and consistent while media, GPU and reload remain future work', () => {
  const summary =
    'The first Linux headless milestone is signed off after source and evidence review and an independent full review.';
  const capability =
    'Assets & Resources prepares validated resource data and loads it asynchronously with bounded memory, dependency tracking and cancellation.';
  const limits =
    'Native scene and mesh/material packages now load into the software renderer. Validation covers CPU/software fixtures only. Other asset types, media importers, GPU resource management, reload and network streaming remain Under Development.';
  for (const name of ['race', 'race/architecture']) {
    const doc = page(name);
    const content = text(doc.querySelector('main'));
    assert.ok(content.includes(summary), name);
    assert.ok(content.includes(capability), name);
    assert.ok(content.includes(limits), name);
    assert.match(
      doc.querySelector('meta[name="description"]').getAttribute('content'),
      /signed-off first Linux headless Scene & World and Assets & Resources milestones/,
    );
    assert.doesNotMatch(
      content,
      /25\/25|6,877|6877|\bTSan\b|ASan|b331d7|5eb89b|MVP complete|Windows complete/i,
    );
    assert.match(content, /only Linux is currently implemented and tested/i);
  }
  const landing = page('race');
  const term = [...landing.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Assets & Resources',
  );
  assert.ok(term);
  assert.equal(text(term.nextElementSibling), `${summary} ${capability} ${limits}`);
  const architecture = page('race/architecture');
  assert.equal(
    text(architecture.querySelector('[data-node="Assets & Resources"] .flow-status')),
    'First Linux headless milestone signed off',
  );
  assert.equal(
    text(architecture.querySelector('[data-node="Graphics & Rendering"] .flow-status')),
    'Under Development',
  );
});

test('RACE development status distinguishes platform targets, future work and availability', () => {
  const doc = page('race');
  const content = text(doc.querySelector('main'));
  const status = text(section(doc, 'Development Status'));
  for (const target of ['Windows', 'Linux', 'macOS', 'Android', 'iOS'])
    assert.ok(status.includes(target), target);
  assert.match(status, /(?:only Linux|Linux (?:is|remains) the only)/i);
  assert.match(status, /(?:later.*Android.*iOS|Android.*iOS.*later)/i);
  assert.match(status, /Under Development/);
  assert.match(content, /no public engine download/i);
  assert.doesNotMatch(
    content,
    /To Be Decided|worlds in motion|Taking shape|Small responsibilities|A core to build around/i,
  );
  assert.doesNotMatch(
    content,
    /benchmark|full-trial|Taskflow|enkiTS|Unreal|Unity|Aver|jobs per second|\bp95\b|\bp99\b|\bFPS\b|\b8K\b/i,
  );
  assert.doesNotMatch(
    doc.documentElement.outerHTML,
    /docs\.google\.com|\/workspace\/|Sentinel_|race-core-spec|github\.com\/ryhzecore|library-file:|libfile_/i,
  );
  assert.equal(doc.querySelectorAll('main a[download], main iframe, main canvas').length, 0);
});

test('3D software milestone sign-off stays bounded and excludes private validation details', () => {
  const doc = page('race');
  const term = [...doc.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Graphics & Rendering',
  );
  assert.ok(term);
  const progress = text(term.nextElementSibling);
  assert.equal(
    progress,
    'The first small software-rendered 3D milestone is signed off after source and evidence review. It includes indexed meshes, camera movement, depth, materials and directional lighting. Scene and resource lifetimes, render completion, readback and bounded memory have been tested. Validation covers CPU/software rendering only. Hardware rendering, ray tracing and path tracing remain future work. Full Graphics & Rendering remains Under Development.',
  );
  assert.equal(text(term.nextElementSibling.querySelector('strong')), 'Under Development');
  assert.doesNotMatch(
    progress,
    /pending|in review|production.ready|shipped|full graphics[^.]*signed off/i,
  );
  assert.doesNotMatch(
    text(doc.querySelector('main')),
    /31\/31|30\/30|44\/44|26 actual draws|depth audits|76afbdf4|08e0be7c|RTX 5090|7680|160 by 120/i,
  );
  assert.doesNotMatch(
    text(doc.querySelector('.race-hero')),
    /without a graphical editor or renderer/,
  );
  const architecture = page('race/architecture');
  assert.equal(
    text(architecture.querySelector('[data-node="Graphics & Rendering"] .flow-status')),
    'Under Development',
  );
});

test('RACE controls retain shared wrapping, radii and minimum targets at narrow and desktop sizes', () => {
  const doc = page('race');
  assert.ok(doc.querySelectorAll('.race-page .button').length > 0);
  for (const viewport of [320, 390, 700, 768, 960, 1440]) {
    for (const button of doc.querySelectorAll('.race-page .button')) {
      const style = cascade(button, viewport);
      // This helper inspects authored layout; actual wrapping/focus remain browser checks.
      assert.equal(button.style.whiteSpace || style['white-space'], 'normal');
      assert.equal(style['border-radius'], '999px');
      assert.equal(style['min-height'], '52px');
      assert.equal(style['font-size'], '0.875rem');
      assert.equal(style['padding-top'], '14px');
      assert.equal(style['padding-bottom'], '14px');
      assert.equal(style['padding-left'], '24px');
      assert.equal(style['padding-right'], '24px');
      assert.notEqual(style.overflow, 'hidden');
    }
    for (const link of doc.querySelectorAll('.race-contents a'))
      assert.equal(cascade(link, viewport)['min-height'], '44px');
    assert.equal(cascade(doc.querySelector('.race-contents'), viewport)['flex-wrap'], 'wrap');
  }
});

test('unfinished public library routes retain truthful current styled availability notices', () => {
  for (const name of ['library', 'film-library', 'saved']) {
    const doc = page(name);
    assert.match(doc.body.textContent, /Not available yet/);
    assert.ok(doc.querySelector('.site-header'));
    assert.ok(doc.querySelector('a[href="/"]'));
    assert.doesNotMatch(doc.body.textContent, /Talk to us|0\.0\.9|Beta/);
  }
});


test('bounded mesh-particle progress leaves broader VFX in development', () => {
  const doc = page('race');
  const term = [...doc.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Future areas',
  );
  assert.ok(term);
  const progress = text(term.nextElementSibling);
  assert.ok(
    progress.includes(
      'Bounded CPU mesh particles now run in the software renderer, with fixed-step motion, repeatable emission and tested lifetimes. Broader VFX and authoring tools remain Under Development.',
    ),
  );
  assert.equal(text(term.nextElementSibling.querySelector('strong')), 'Under Development');
  assert.doesNotMatch(progress, /45\/45|44\/44|0be1e19e|b9f81527|shipped|GPU simulation|complete VFX|https?:/i);
});

test('bounded sphere physics progress leaves broader simulation in development', () => {
  const doc = page('race');
  const term = [...doc.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Future areas',
  );
  assert.ok(term);
  assert.ok(
    text(term.nextElementSibling).includes(
      'Bounded CPU physics now supports spheres and a static plane, with fixed-step motion, collision response and saved-state recovery. Broader physics and simulation remain Under Development.',
    ),
  );
});

test('bounded offline audio progress leaves device and spatial audio in development', () => {
  const doc = page('race');
  const term = [...doc.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Future areas',
  );
  assert.ok(term);
  assert.ok(
    text(term.nextElementSibling).includes(
      'Bounded offline audio now supports native sound assets and collision cues. Device playback and spatial audio remain Under Development.',
    ),
  );
});

test('native CPU atmosphere progress leaves Vulkan, weather and celestial content in development', () => {
  const doc = page('race');
  const term = [...doc.querySelectorAll('.race-status-list dt')].find(
    (node) => text(node) === 'Future areas',
  );
  assert.ok(term);
  const progress = text(term.nextElementSibling);
  assert.ok(
    progress.includes(
      'A bounded native CPU atmosphere reference now models sky scattering, sunlight attenuation and distance haze. Vulkan atmosphere rendering, weather and celestial content remain Under Development.',
    ),
  );
  assert.equal(text(term.nextElementSibling.querySelector('strong')), 'Under Development');
  assert.doesNotMatch(progress, /61\/61|469|8b38fa3d|fee92514|https?:|8K60|5090|shipped/i);
});
