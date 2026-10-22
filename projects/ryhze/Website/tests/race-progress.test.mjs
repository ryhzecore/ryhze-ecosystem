import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync, readdirSync } from 'node:fs';
import { parseHTML } from 'linkedom';

const origin = 'https://preview.example';
const retired = ['/race/progress/', '/race/progress/scene/'];
const document = (name) =>
  parseHTML(readFileSync(new URL(`../dist/${name}/index.html`, import.meta.url), 'utf8')).document;

test('retired progress pages have same-origin static redirect fallbacks to the RACE landing', () => {
  for (const path of retired) {
    const doc = document(path.slice(1, -1));
    const refresh = doc.querySelector('meta[http-equiv="refresh"]');
    assert.ok(refresh, `${path} needs a static-preview redirect`);
    const match = /^(\d+);\s*url=(.+)$/i.exec(refresh.getAttribute('content'));
    assert.ok(match);
    const target = new URL(match[2], new URL(path, origin));
    assert.equal(target.origin, origin);
    assert.equal(target.pathname, '/race/');
    assert.equal(target.search, '');
    assert.equal(target.hash, '');
    assert.ok(
      [...doc.querySelectorAll('a[href]')].some(
        (link) => new URL(link.getAttribute('href'), new URL(path, origin)).href === target.href,
      ),
      'A usable fallback link is required when the browser does not follow meta refresh',
    );
    assert.equal(
      doc.querySelectorAll('[data-benchmark-row], figure, canvas, iframe, script').length,
      0,
    );
    assert.doesNotMatch(
      doc.documentElement.textContent,
      /full-trial|gate[s]? passed|p95|p99|MiB|jobs per second/i,
    );
  }
});

test('built public pages contain no retired report links or benchmark presentation', () => {
  const root = new URL('../dist/', import.meta.url);
  const htmlFiles = readdirSync(root, { recursive: true }).filter((name) => name.endsWith('.html'));
  assert.ok(htmlFiles.includes('race/index.html'));
  for (const name of htmlFiles) {
    const doc = parseHTML(readFileSync(new URL(name, root), 'utf8')).document;
    assert.equal(doc.querySelectorAll('[data-benchmark-row]').length, 0, name);
    for (const link of doc.querySelectorAll('a[href]')) {
      const url = new URL(link.getAttribute('href'), new URL(name, origin));
      assert.ok(!/^\/race\/progress(?:\/|$)/.test(url.pathname), `${name}: ${url.pathname}`);
    }
  }
  const content = document('race').body.textContent.replace(/\s+/g, ' ');
  assert.doesNotMatch(
    content,
    /benchmark|full-trial|Taskflow|enkiTS|Unreal|Unity|Aver|jobs per second|\bp95\b|\bp99\b/i,
  );
});
