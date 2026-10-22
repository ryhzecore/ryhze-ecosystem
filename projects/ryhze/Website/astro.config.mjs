import { defineConfig } from 'astro/config';

export default defineConfig({
  site: 'https://ryhze.com',
  output: 'static',
  trailingSlash: 'always',
  // Static-preview fallback; the production Worker supplies the HTTP redirect
  // and preserves query strings for these retired public report URLs.
  redirects: {
    '/race/progress': { status: 302, destination: '/race/' },
    '/race/progress/scene': { status: 302, destination: '/race/' },
  },
  devToolbar: { enabled: false },
  vite: {
    build: {
      assetsInlineLimit: 0,
      // Keep shared UI together for compression; the one page entry imports it.
      rollupOptions: {
        output: {
          chunkFileNames: '[hash].js',
          manualChunks(id) {
            if (
              /\/src\/scripts\/(ui|navigation|detail-frame|frame-motion|platform-frames|skip-link|ambient|title-dialog|entertainment|entertainment-search|category-drag)\.ts$/.test(
                id,
              )
            )
              return 'ui';
          },
        },
      },
      minify: 'terser',
      terserOptions: {
        ecma: 2020,
        compress: { passes: 4, ecma: 2020 },
        format: { comments: false },
      },
    },
  },
});
