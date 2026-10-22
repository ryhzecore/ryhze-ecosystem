import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import { parseHTML } from 'linkedom';
import ts from 'typescript';
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8');
const context = { exports: {} };
vm.runInNewContext(
  ts.transpileModule(read('src/scripts/entertainment-search.ts'), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
  }).outputText,
  context,
);

for (const page of ['games', 'films']) {
  test(`${page}: local title search handles case, empty queries and markup-like input safely`, () => {
    const { document, window } = parseHTML(read(`dist/${page}/index.html`));
    const cleanup = context.exports.initializeSearch(document);
    const input = document.querySelector('[data-search-input]');
    const result = document.querySelector('[data-search-title]');
    const film = document.querySelector('[data-search-title="tommy riley"]');
    const empty = document.querySelector('[data-search-empty]');
    for (const [query, visible] of [
      ['  LARCENOUS ', true],
      ['nothing', false],
      ['<img onerror=alert(1)>', false],
      ['', true],
    ]) {
      input.value = query;
      input.dispatchEvent(new window.Event('input'));
      assert.equal(result.hidden, !visible);
      assert.equal(empty.hidden, visible);
      assert.equal(document.querySelector('[data-search] img'), null);
    }
    assert.equal(result.getAttribute('href'), '/games/#game-details');
    input.value = '  TOMMY RILEY ';
    input.dispatchEvent(new window.Event('input'));
    assert.equal(result.hidden, true);
    assert.equal(film.hidden, false);
    assert.equal(empty.hidden, true);
    assert.equal(film.getAttribute('href'), '/films/#film-details');
    assert.match(film.textContent, /Film · In development/);
    cleanup();
  });
}

test('one actual exclusive has a readable counter and no redundant carousel controls', () => {
  const doc = parseHTML(read('dist/games/index.html')).document;
  const showcase = doc.querySelector('[data-exclusive-showcase]');
  assert.equal(showcase.getAttribute('aria-label'), 'Ryhze Exclusives');
  assert.equal(showcase.querySelectorAll('[data-exclusive-slide]').length, 1);
  assert.equal(showcase.querySelectorAll('[data-exclusive-step]').length, 0);
  assert.ok(showcase.querySelector('[aria-label="Featured title 1 of 1"]'));
});
