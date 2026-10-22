import assert from 'node:assert/strict';
import test from 'node:test';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
const require = createRequire(import.meta.url);
const wranglerRequire = createRequire(require.resolve('wrangler/package.json'));
const { parseRoutes, matchRoutes } = await import(
  pathToFileURL(wranglerRequire.resolve('miniflare')).href
);

test('official local route parser prefers explicit schemes independently of declaration order', () => {
  for (const reversed of [false, true]) {
    const definitions = [
      ['legacy', ['ryhze.com/*', 'www.ryhze.com/*', 'video.ryhze.com/*']],
      [
        'frontend',
        [
          'https://ryhze.com/*',
          'http://ryhze.com/*',
          'https://www.ryhze.com/*',
          'http://www.ryhze.com/*',
        ],
      ],
    ];
    const routes = parseRoutes(new Map(reversed ? definitions.reverse() : definitions));
    for (const scheme of ['https', 'http']) {
      for (const host of ['ryhze.com', 'www.ryhze.com'])
        for (const path of ['/', '/?q=test', '/entertainment/', '/legacy-unknown']) {
          assert.equal(matchRoutes(routes, new URL(`${scheme}://${host}${path}`)), 'frontend');
        }
      assert.equal(matchRoutes(routes, new URL(`${scheme}://video.ryhze.com/`)), 'legacy');
      assert.equal(matchRoutes(routes, new URL(`${scheme}://other.ryhze.com/`)), null);
    }
  }
});
