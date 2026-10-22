import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';

test('production overlay is limited to exact apex/www HTTP and HTTPS routes', async () => {
  const config = JSON.parse(await readFile(new URL('../wrangler.json', import.meta.url), 'utf8'));
  assert.deepEqual(
    config.routes.map((r) => r.pattern).sort(),
    [
      'http://ryhze.com/*',
      'http://www.ryhze.com/*',
      'https://ryhze.com/*',
      'https://www.ryhze.com/*',
    ].sort(),
  );
  assert.ok(config.routes.every((r) => r.zone_name === 'ryhze.com' && !r.custom_domain));
  assert.deepEqual(config.services, [{ binding: 'LEGACY', service: 'ryhze-web' }]);
  assert.equal(config.workers_dev, false);
  assert.equal(config.preview_urls, false);
  assert.equal(config.observability.enabled, false);
  assert.equal(config.assets.binding, 'ASSETS');
  assert.equal(config.assets.run_worker_first, true);
  for (const field of [
    'd1_databases',
    'r2_buckets',
    'kv_namespaces',
    'vars',
    'secrets',
    'durable_objects',
    'queues',
  ])
    assert.equal(config[field], undefined);
});
test('frontend allowlist has no login/private server files or broad wildcard prefixes', async () => {
  const paths = JSON.parse(await readFile(new URL('../paths.json', import.meta.url), 'utf8'));
  assert.ok(
    paths.every(
      (p) =>
        !p.includes('*') &&
        !/^\/(?:__auth|__gate|__agreements|hosting|gate|auth|node_modules|\.env)(?:\/|$)/.test(p),
    ),
  );
  assert.ok(paths.includes('/'));
  assert.ok(paths.includes('/entertainment'));
  assert.ok(paths.includes('/entertainment/'));
});
