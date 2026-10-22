import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
const require = createRequire(import.meta.url);
const clerkPath = dirname(require.resolve('@clerk/clerk-js'));
const sdk = await readFile(join(clerkPath, 'clerk.browser.js'), 'utf8');
await mkdir(new URL('../auth/generated/', import.meta.url), { recursive: true });
await writeFile(
  new URL('../auth/generated/clerk-sdk.mjs', import.meta.url),
  `// Generated from locked @clerk/clerk-js; never edit.\nexport default ${JSON.stringify(sdk)};\n`,
);
console.log('Built pinned Clerk authentication bootstrap (no configuration or secrets).');
