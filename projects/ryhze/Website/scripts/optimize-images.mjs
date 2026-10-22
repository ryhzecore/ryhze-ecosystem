/** Recreate delivery-only derivatives. Original PNGs remain unchanged. */
import { mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';
const root = fileURLToPath(new URL('../public/brand/', import.meta.url));
await mkdir(`${root}web`, { recursive: true });
for (const [name, widths] of [
  ['wordmark', [174, 348, 522]],
  ['symbol', [240, 480, 720, 1080]],
]) {
  for (const width of widths) {
    await sharp(`${root}${name}.png`)
      .resize({ width, withoutEnlargement: true })
      .webp({ lossless: true, effort: 6 })
      .toFile(`${root}web/${name}-${width}.webp`);
  }
}
for (const size of [32, 48]) {
  await sharp(`${root}app-icon.png`)
    .resize(size, size)
    .png({ compressionLevel: 9 })
    .toFile(`${root}web/favicon-${size}.png`);
}
