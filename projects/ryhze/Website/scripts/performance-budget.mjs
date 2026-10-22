import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { extname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { parseHTML } from 'linkedom';

const root = fileURLToPath(new URL('../', import.meta.url));
export const budgets = {
  javascriptGzip: 5 * 1024,
  stylesheetsGzip: 10 * 1024,
  eachHtmlGzip: 10 * 1024,
  referencedFonts: 416 * 1024,
  eachResponsiveImage: 24 * 1024,
  eachFavicon: 2 * 1024,
  homepageReferenceResources: 500 * 1024,
};
function filesWithin(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = join(directory, entry.name);
    return entry.isDirectory() ? filesWithin(path) : [path];
  });
}
export function measure(directory) {
  const paths = filesWithin(directory);
  const bytes = (path) => statSync(path).size;
  const records = (extension) =>
    paths
      .filter((path) => extname(path) === extension)
      .map((path) => ({
        path: relative(directory, path).replaceAll('\\', '/'),
        bytes: bytes(path),
        gzipBytes: gzipSync(readFileSync(path), { level: 9 }).byteLength,
      }));
  const javascript = records('.js');
  const stylesheets = records('.css');
  const html = records('.html');
  const local = (url) => {
    assert.ok(url.startsWith('/') && !url.startsWith('//'), `Expected a local asset: ${url}`);
    const path = resolve(directory, `.${url}`);
    assert.ok(path.startsWith(`${resolve(directory)}${sep}`), `Asset escapes output: ${url}`);
    return path;
  };
  const fontPaths = new Set();
  for (const record of stylesheets) {
    for (const match of readFileSync(join(directory, record.path), 'utf8').matchAll(
      /url\(\s*['"]?([^)'"\s]+)['"]?\s*\)/g,
    )) {
      if (/\.woff2?$/.test(match[1])) fontPaths.add(match[1]);
    }
  }
  const referencedFonts = [...fontPaths].map((path) => ({ path, bytes: bytes(local(path)) }));
  const document = parseHTML(readFileSync(join(directory, 'index.html'), 'utf8')).document;
  const resourcePaths = new Set(['/index.html']);
  for (const node of document.querySelectorAll(
    'script[src], link[rel="stylesheet"], link[as="font"]',
  )) {
    resourcePaths.add(node.getAttribute('src') || node.getAttribute('href'));
  }
  for (const { path } of referencedFonts) resourcePaths.add(path);
  for (const img of document.querySelectorAll('img')) {
    const candidates = [
      img.getAttribute('src'),
      ...(img.getAttribute('srcset') || '').split(',').map((item) => item.trim().split(/\s+/)[0]),
    ].filter(Boolean);
    resourcePaths.add(candidates.sort((a, b) => bytes(local(b)) - bytes(local(a)))[0]);
  }
  const icons = [...document.querySelectorAll('link[rel="icon"]')].map((node) =>
    node.getAttribute('href'),
  );
  if (icons.length) resourcePaths.add(icons.sort((a, b) => bytes(local(b)) - bytes(local(a)))[0]);
  const homepageResources = [...resourcePaths]
    .sort()
    .map((path) => ({ path, bytes: bytes(local(path)) }));
  return {
    method:
      'Offline file sizes. gzip is level 9, not observed HTTP compression. Homepage reference sum counts the largest candidate per image plus one largest favicon, shared assets once, all referenced fonts, HTML, CSS and JS. It includes the lazy footer image; it is not a strict request upper bound, browser trace, timing or Web Vitals result. Different header/footer candidates or viewport changes can add image requests.',
    javascript,
    stylesheets,
    html,
    referencedFonts,
    homepageResources,
    totals: {
      javascript: javascript.reduce((sum, entry) => sum + entry.bytes, 0),
      javascriptGzip: javascript.reduce((sum, entry) => sum + entry.gzipBytes, 0),
      stylesheets: stylesheets.reduce((sum, entry) => sum + entry.bytes, 0),
      stylesheetsGzip: stylesheets.reduce((sum, entry) => sum + entry.gzipBytes, 0),
      referencedFonts: referencedFonts.reduce((sum, entry) => sum + entry.bytes, 0),
      homepageReferenceResources: homepageResources.reduce((sum, entry) => sum + entry.bytes, 0),
      entireStaticOutput: paths.reduce((sum, path) => sum + bytes(path), 0),
    },
  };
}
export function enforce(report, directory) {
  for (const name of [
    'javascriptGzip',
    'stylesheetsGzip',
    'referencedFonts',
    'homepageReferenceResources',
  ]) {
    assert.ok(
      report.totals[name] <= budgets[name],
      `${name}: ${report.totals[name]} exceeds ${budgets[name]} bytes`,
    );
  }
  for (const page of report.html)
    assert.ok(page.gzipBytes <= budgets.eachHtmlGzip, `${page.path}: HTML gzip budget exceeded`);
  for (const path of filesWithin(join(directory, 'brand/web'))) {
    const limit = path.endsWith('.webp') ? budgets.eachResponsiveImage : budgets.eachFavicon;
    assert.ok(statSync(path).size <= limit, `${relative(directory, path)}: image budget exceeded`);
  }
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const directory = join(root, 'dist');
  const report = measure(directory);
  enforce(report, directory);
  const baselineArgument = process.argv.indexOf('--compare');
  console.log(
    JSON.stringify(
      {
        budgets,
        current: report,
        ...(baselineArgument >= 0
          ? { baseline: measure(resolve(process.argv[baselineArgument + 1])) }
          : {}),
      },
      null,
      2,
    ),
  );
}
