import { defineConfig } from 'astro/config';
import { fileURLToPath } from 'node:url';

// Separate static specimen build. Never included in the public production dist.
export default defineConfig({
  root: fileURLToPath(new URL('./', import.meta.url)),
  srcDir: fileURLToPath(new URL('./src/', import.meta.url)),
  publicDir: fileURLToPath(new URL('../../public/', import.meta.url)),
  outDir: fileURLToPath(new URL('../../.local/account-preview/', import.meta.url)),
  cacheDir: fileURLToPath(new URL('../../.local/account-astro/', import.meta.url)),
  output: 'static',
  trailingSlash: 'always',
  devToolbar: { enabled: false },
  vite: { build: { assetsInlineLimit: 0 } },
});
