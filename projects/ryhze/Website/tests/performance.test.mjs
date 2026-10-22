import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { parseHTML } from 'linkedom';
import sharp from 'sharp';
import { budgets, enforce, measure } from '../scripts/performance-budget.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const read = (path) => readFileSync(`${root}${path}`);
const sha256 = (path) => createHash('sha256').update(read(path)).digest('hex');
const originals = {
  'brand/app-icon.png': '81e8d6c0c156eda01d5670ea1c2fc6d77ffc13b3698f7ce7ab7680d666e82605',
  'brand/wordmark.png': '9f4d038e3fd2fb7cc96a6617621755e5e0119b35acebe6825b98571d4cec16e4',
  'brand/symbol.png': 'e3dd110965bc7f67d5a8cae4f57a83e00d165067246c35ba439eeb33e42c40a8',
  'fonts/Inter.woff': '865a0c76d2bc7fc6b2ed4cd72d9b1c1ea7b81b4dcb7be005db3c883610560d3b',
  'fonts/SpaceGrotesk.woff': 'af57917fd4230959955c202bce2d281c56c974f343f2983f446d22df78d554c7',
  'fonts/Inter-LICENSE.txt': '5b9321a4298cfeb6b34354164a1c3afc3db114569984c502b9b35d988fd58c57',
  'fonts/SpaceGrotesk-LICENSE.txt':
    'c6dec685825f73b18c20926fddc65e8315642e12986f15db0699170940a09efc',
};

test('original brand artwork, licensed fonts and OFL notices retain their baseline bytes', () => {
  for (const [path, expected] of Object.entries(originals)) {
    assert.equal(sha256(`public/${path}`), expected, path);
    assert.equal(sha256(`dist/${path}`), expected, path);
  }
});

test('checked-in WOFF2 outputs match the complete-table conversion verification', () => {
  const records = JSON.parse(read('docs/font-encoding.json'));
  assert.equal(records.length, 2);
  for (const record of records) {
    assert.equal(sha256(`public/fonts/${record.family}.woff`), record.sourceSha256);
    assert.equal(sha256(`public/fonts/${record.family}.woff2`), record.woff2Sha256);
    assert.equal(sha256(`dist/fonts/${record.family}.woff2`), record.woff2Sha256);
    assert.ok(record.glyphs > 600);
    assert.ok(record.axes.some((axis) => axis.tag === 'wght'));
    for (const table of ['glyf', 'loca', 'cmap', 'hmtx', 'name', 'fvar', 'gvar'])
      assert.ok(record.byteIdenticalTables.includes(table));
  }
});

for (const [name, widths] of [
  ['wordmark', [174, 348, 522]],
  ['symbol', [240, 480, 720, 1080]],
]) {
  test(`${name}: responsive lossless images preserve the resized original pixels and alpha`, async () => {
    const original = `${root}public/brand/${name}.png`;
    const sourceMetadata = await sharp(original).metadata();
    for (const width of widths) {
      const path = `${root}public/brand/web/${name}-${width}.webp`;
      const metadata = await sharp(path).metadata();
      assert.equal(metadata.width, width);
      assert.equal(
        metadata.height,
        Math.round((width * sourceMetadata.height) / sourceMetadata.width),
      );
      assert.equal(metadata.hasAlpha, true);
      const reference = await sharp(original)
        .resize({ width, withoutEnlargement: true })
        .ensureAlpha()
        .raw()
        .toBuffer();
      const actual = await sharp(path).ensureAlpha().raw().toBuffer();
      assert.equal(actual.length, reference.length);
      for (let index = 0; index < actual.length; index += 4) {
        assert.equal(actual[index + 3], reference[index + 3], `alpha at pixel ${index / 4}`);
        if (reference[index + 3] !== 0)
          assert.deepEqual(actual.subarray(index, index + 3), reference.subarray(index, index + 3));
      }
    }
  });
}

test('the small favicons contain only exact resized original artwork', async () => {
  for (const size of [32, 48]) {
    const actual = await sharp(`${root}public/brand/web/favicon-${size}.png`).raw().toBuffer();
    const reference = await sharp(`${root}public/brand/app-icon.png`)
      .resize(size, size)
      .raw()
      .toBuffer();
    assert.deepEqual(actual, reference);
  }
});

test('delivery references use responsive images, matched WOFF2 preloads and a lazy footer', () => {
  const document = parseHTML(read('dist/index.html').toString()).document;
  const logos = [...document.querySelectorAll('.brand img')];
  assert.equal(logos.length, 2);
  for (const image of logos) {
    assert.equal(image.getAttribute('srcset').split(',').length, 3);
    assert.equal(image.getAttribute('width'), '7256');
    assert.equal(image.getAttribute('height'), '2480');
  }
  assert.equal(logos[0].getAttribute('loading'), 'eager');
  assert.equal(logos[1].getAttribute('loading'), 'lazy');
  const symbol = document.querySelector('.stage-symbol');
  assert.equal(symbol.getAttribute('srcset').split(',').length, 4);
  assert.equal(symbol.getAttribute('fetchpriority'), 'high');
  assert.equal(symbol.getAttribute('width'), symbol.getAttribute('height'));
  for (const link of document.querySelectorAll('link[as="font"]')) {
    assert.ok(link.getAttribute('href').endsWith('.woff2'));
    assert.equal(link.getAttribute('type'), 'font/woff2');
    assert.ok(link.hasAttribute('crossorigin'));
  }
  assert.equal(document.querySelectorAll('link[as="font"]').length, 2);
  assert.equal(document.querySelectorAll('astro-island').length, 0);
});

test('production output stays inside the measured byte budgets', () => {
  enforce(measure(`${root}dist`), `${root}dist`);
});

test('budget enforcement rejects an oversized JavaScript report', () => {
  const report = measure(`${root}dist`);
  report.totals.javascriptGzip = budgets.javascriptGzip + 1;
  assert.throws(() => enforce(report, `${root}dist`), /javascriptGzip/);
});
