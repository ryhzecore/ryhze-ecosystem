import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import test from 'node:test';
import { parseHTML } from 'linkedom';

const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const rule = (css, selector) => css.slice(css.indexOf(`${selector} {`)).split('}')[0];

test('all authored interface colors are monochrome, including the account preview', () => {
  const sheets = readdirSync(new URL('../src/styles/', import.meta.url)).map(
    (file) => `src/styles/${file}`,
  );
  sheets.push('preview/account/src/styles/account.css');
  for (const path of sheets) {
    for (const [, hex] of read(path).matchAll(
      /#([0-9a-f]{8}|[0-9a-f]{6}|[0-9a-f]{4}|[0-9a-f]{3})\b/gi,
    )) {
      const rgb = hex.length <= 4 ? [...hex.slice(0, 3)] : hex.slice(0, 6).match(/../g);
      assert.equal(new Set(rgb).size, 1, `${path}: #${hex}`);
    }
    assert.doesNotMatch(read(path), /var\(--color-blue/);
  }
});

test('white buttons and the stage badge share dark ink while disabled actions stay neutral', () => {
  const css = read('src/styles/components.css');
  for (const selector of ['.button--primary', '.brand-stage-tag']) {
    const styles = rule(css, selector);
    assert.match(styles, /var\(--color-primary\)/);
    assert.match(styles, /color: var\(--color-on-primary\)/);
  }
  assert.match(rule(css, '.button--primary:hover'), /var\(--color-primary-hover\)/);
  assert.match(rule(css, '.button:disabled:hover'), /color: var\(--color-subtle\)/);
  const preview = read('preview/account/src/styles/account.css');
  assert.match(rule(preview, '.active-link'), /color: var\(--color-on-primary\)/);
  assert.match(rule(preview, '.primary:disabled'), /var\(--color-surface-raised\)/);
});

test('category ink follows the moving selection without adding duplicate links or JS paint work', () => {
  const css = read('src/styles/entertainment.css');
  assert.match(css, /@supports \(mix-blend-mode: difference\)/);
  assert.match(css, /color: #fff;\s*mix-blend-mode: difference/);
  assert.match(
    css,
    /@media \(forced-colors: active\) \{\s*\.entertainment-chooser a,\s*\.entertainment-chooser a:hover,\s*\.entertainment-chooser a\[aria-current='page'\] \{\s*mix-blend-mode: normal/,
  );
  const forced = css.slice(css.indexOf('@media (forced-colors: active)'));
  assert.match(rule(forced, '.entertainment-selection'), /visibility: hidden/);
  assert.match(
    forced,
    /\.entertainment-chooser a\[aria-current='page'\] \{\s*background: Highlight;\s*color: HighlightText/,
  );
  assert.match(
    css,
    /\.entertainment-chooser\[data-dragging\] \.entertainment-selection \{\s*transition: none/,
  );
  for (const category of ['games', 'films']) {
    const doc = parseHTML(read(`dist/${category}/index.html`)).document;
    const chooser = doc.querySelector('[data-entertainment-chooser]');
    assert.equal(chooser.querySelectorAll('a').length, 2);
    assert.equal(chooser.querySelectorAll('[aria-current="page"]').length, 1);
    assert.equal(
      chooser.querySelector('.entertainment-selection').getAttribute('aria-hidden'),
      'true',
    );
  }
});

test('account preview has no glow and matches the black browser chrome', () => {
  const doc = parseHTML(read('.local/account-preview/account-preview/index.html')).document;
  assert.equal(doc.querySelector('.account-ambient'), null);
  assert.equal(doc.querySelector('meta[name="theme-color"]').getAttribute('content'), '#000000');
});
