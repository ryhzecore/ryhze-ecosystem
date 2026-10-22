import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { join } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));

/** The preparation checkpoint must be inert even if Wrangler is invoked. */
export function assertSafePreparation(config) {
  const allowedKeys = [
    '$schema',
    'name',
    'main',
    'compatibility_date',
    'workers_dev',
    'preview_urls',
    'assets',
    'vars',
    'observability',
    'send_metrics',
  ];
  assert.ok(
    Object.keys(config).every((key) => allowedKeys.includes(key)),
    'Unreviewed top-level hosting setting',
  );
  assert.equal(
    config.name,
    'ryhze-website-tester-v009',
    'Use only the separate tester-preparation Worker',
  );
  assert.equal(config.main, 'hosting/tester-worker.mjs');
  assert.equal(config.workers_dev, false, 'Do not expose workers.dev before Access verification');
  assert.equal(config.preview_urls, false, 'Version/preview URLs are public unless protected');
  assert.equal(config.assets?.directory, './dist');
  assert.equal(config.assets?.binding, 'ASSETS');
  assert.equal(config.assets?.run_worker_first, true, 'Every asset must pass the server gate');
  assert.equal(config.assets?.html_handling, 'force-trailing-slash');
  assert.equal(config.assets?.not_found_handling, '404-page');
  assert.equal(config.observability?.enabled, false, 'Do not record identity-bearing request logs');
  assert.deepEqual(Object.keys(config.vars).sort(), ['RYHZE_RELEASE', 'RYHZE_STAGE']);
  assert.equal(config.vars.RYHZE_RELEASE, '0.0.9');
  assert.equal(config.vars.RYHZE_STAGE, 'invited-accounts-preparation');
  for (const key of [
    'account_id',
    'route',
    'routes',
    'env',
    'd1_databases',
    'r2_buckets',
    'kv_namespaces',
    'durable_objects',
    'services',
    'triggers',
    'unsafe',
    'access',
  ]) {
    assert.equal(config[key], undefined, `Unapproved deployment binding/configuration: ${key}`);
  }
}

async function pathsUnder(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  return (
    await Promise.all(
      entries.map(async (entry) => {
        const path = join(directory, entry.name);
        assert.ok(!entry.isSymbolicLink(), `No symlinks in uploaded assets: ${path}`);
        return entry.isDirectory() ? pathsUnder(path) : [path];
      }),
    )
  ).flat();
}

export async function runPreflight(directory = root) {
  const config = JSON.parse(await readFile(join(directory, 'wrangler.json'), 'utf8'));
  assertSafePreparation(config);
  const files = await pathsUnder(join(directory, 'dist'));
  assert.ok(files.length > 0 && files.length <= 20000);
  for (const path of files) {
    assert.ok(
      !/(?:^|\/)(?:gate|hosting|node_modules|\.git|\.env|\.dev\.vars)(?:[/.]|$)|\.(?:map|sql)$/i.test(
        path,
      ),
      `Server/private material found in static output: ${path}`,
    );
  }
  console.log(
    `Hosting preparation preflight passed: ${files.length} assets; all external URLs disabled.`,
  );
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  await runPreflight();
