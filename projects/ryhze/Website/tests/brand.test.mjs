import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { parseHTML } from 'linkedom';

const root = fileURLToPath(new URL('../', import.meta.url));
const read = (path) => readFileSync(`${root}${path}`, 'utf8');

for (const page of [
  'index.html',
  'downloads/index.html',
  'entertainment/index.html',
  'contact/index.html',
  'privacy/index.html',
  'legal/index.html',
  '404.html',
]) {
  test(`${page}: header shows PRE-ALPHA after the unchanged logo`, () => {
    const document = parseHTML(read(`dist/${page}`)).document;
    const brand = document.querySelector('.site-header .brand');
    const logo = brand.querySelector('img');
    const badge = brand.querySelector('.brand-stage-tag');
    assert.equal(logo.getAttribute('src'), '/brand/web/wordmark-348.webp');
    assert.equal(logo.nextElementSibling, badge);
    assert.equal(badge.textContent, 'PRE-ALPHA');
    assert.equal(document.querySelectorAll('.beta-tag').length, 0);
    assert.equal(document.querySelectorAll('.brand--footer .brand-stage-tag').length, 0);
  });
}

test('header badge stays in a padded, centered row with mobile shrink space', () => {
  const css = read('src/styles/components.css');
  const brand = css.match(/\.brand:not\(\.brand--footer\) \{([^}]+)\}/)[1];
  const badge = css.match(/\.brand-stage-tag \{([^}]+)\}/)[1];
  assert.match(brand, /display: inline-flex/);
  assert.match(brand, /align-items: center/);
  assert.match(brand, /gap: 12px/);
  assert.match(badge, /padding: 6px 10px/);
  assert.match(badge, /white-space: nowrap/);
  assert.doesNotMatch(badge, /position: absolute|top:|left:/);
  assert.match(css, /max-width: calc\(100% - 58px\)/);
});
