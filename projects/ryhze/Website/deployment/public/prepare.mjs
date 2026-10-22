import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { lstat, readFile, readdir, writeFile } from 'node:fs/promises';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { resolve, relative, sep } from 'node:path';

// Only the build-generated Astro namespace can receive immutable caching.
// Public-directory copies cannot prove a content-addressed name, even if their
// filenames resemble a generated bundle. Keep the shape in sync with worker.
const HASHED_BUNDLE =
  /^\/_astro\/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.[A-Za-z0-9_-]{8,64}\.(?:css|js|mjs)$/;
const PRIVATE_FILE =
  /(?:^|\/)(?:api|__auth|__gate|__agreements|hosting|gate|auth|portal|private|account|login|logout|oauth|session|node_modules|\.git|\.env|\.dev\.vars)(?:\/|\.|$)|\.(?:map|sql|toml)$/i;

async function readPreviousManifest(path) {
  try {
    const manifest = JSON.parse(await readFile(path, 'utf8'));
    assert.equal(manifest.schemaVersion, 1, 'Unexpected cache manifest schema');
    assert.ok(Array.isArray(manifest.immutableAssets), 'Invalid cache asset manifest');
    return new Map(manifest.immutableAssets.map((asset) => [asset.path, asset]));
  } catch (error) {
    if (error.code === 'ENOENT') return new Map();
    throw error;
  }
}

export async function preparePublicAssets({
  root,
  outputDirectory = resolve(root, 'deployment/public'),
}) {
  const dist = resolve(root, 'dist');
  // Vite copies public/ verbatim. Reserve /_astro for the compiler rather than
  // granting immutable caching to a manually named public/_astro file.
  let reservedDirectory;
  try {
    reservedDirectory = await lstat(resolve(root, 'public/_astro'));
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  assert.ok(!reservedDirectory, 'public/_astro is reserved for generated fingerprinted bundles');
  const files = [];
  async function walk(directory) {
    for (const entry of await readdir(directory, { withFileTypes: true })) {
      assert.ok(!entry.isSymbolicLink(), 'Production assets must not contain symlinks');
      const path = resolve(directory, entry.name);
      if (entry.isDirectory()) await walk(path);
      else {
        assert.ok(entry.isFile(), 'Only regular production asset files are supported');
        const name = relative(dist, path).split(sep).join('/');
        assert.ok(!PRIVATE_FILE.test(name), `Private/server file in production assets: ${name}`);
        // Reject ambiguous URL spellings and asset-routing control files. These
        // must never silently change which response an exact manifest path serves.
        assert.ok(
          name.split('/').every((part) => /^[A-Za-z0-9_-][A-Za-z0-9_.-]*$/.test(part)) &&
            !/(?:^|\/)(?:_headers|_redirects|_worker\.js)$/.test(name),
          `Unsupported public asset path: ${name}`,
        );
        files.push(name);
      }
    }
  }
  await walk(dist);
  assert.ok(files.length > 0 && files.length <= 20000, 'Unexpected production asset count');
  const paths = new Set();
  const immutableAssets = [];
  const manifestPath = resolve(outputDirectory, 'cache-manifest.json');
  const previousAssets = await readPreviousManifest(manifestPath);
  for (const file of files.sort()) {
    const path = `/${file}`;
    paths.add(path);
    if (path.endsWith('/index.html')) {
      const directory = path.slice(0, -'index.html'.length);
      paths.add(directory);
      if (directory !== '/') paths.add(directory.slice(0, -1));
    }
    if (HASHED_BUNDLE.test(path)) {
      const bytes = await readFile(resolve(dist, file));
      assert.ok(bytes.length > 0, `Empty fingerprinted bundle: ${path}`);
      const asset = {
        path,
        sha256: createHash('sha256').update(bytes).digest('hex'),
        bytes: bytes.length,
      };
      const previous = previousAssets.get(path);
      assert.ok(
        !previous || (previous.sha256 === asset.sha256 && previous.bytes === asset.bytes),
        `Fingerprinted URL reused for changed bytes: ${path}. Rebuild with a new content hash.`,
      );
      immutableAssets.push(asset);
    }
  }
  // Write only after the complete inventory has passed every safety check.
  await writeFile(
    resolve(outputDirectory, 'paths.json'),
    `${JSON.stringify([...paths].sort(), null, 2)}\n`,
  );
  await writeFile(
    manifestPath,
    `${JSON.stringify({ schemaVersion: 1, immutableAssets }, null, 2)}\n`,
  );
  return { paths: paths.size, files: files.length, immutableAssets: immutableAssets.length };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const root = fileURLToPath(new URL('../../', import.meta.url));
  const result = await preparePublicAssets({ root });
  console.log(
    `Prepared ${result.paths} exact frontend paths from ${result.files} static files; ${result.immutableAssets} verified fingerprinted bundles; no remote action performed.`,
  );
}
