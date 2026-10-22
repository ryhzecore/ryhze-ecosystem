import { bootSessionGuard } from './ui/session-guard.mjs';
import { authCsp } from './ui/pages.mjs';
export const returnPaths = new Set([
  '/',
  '/downloads/',
  '/entertainment/',
  '/games/',
  '/films/',
  '/contact/',
  '/privacy/',
  '/legal/',
  '/__agreements',
]);
export function safeReturnPath(value) {
  return returnPaths.has(value) ? value : '/';
}
const escapeAttribute = (value) =>
  String(value).replace(
    /[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c],
  );
export function sessionTags(config, nonce, path) {
  const settings = JSON.stringify({
    origin: config.origin,
    nonce,
    returnTo: safeReturnPath(path),
    reviewingAgreements: path.startsWith('/__agreements'),
  }).replaceAll('<', '\\u003c');
  return `<script defer nonce="${nonce}" src="/__auth/clerk.js" data-clerk-publishable-key="${escapeAttribute(config.publishableKey)}"></script><script nonce="${nonce}">window.addEventListener('DOMContentLoaded',()=>(${bootSessionGuard.toString()})(${settings}));</script>`;
}
export async function attachDocumentSession(response, config, path) {
  if (
    response.status !== 200 ||
    !response.headers.get('Content-Type')?.toLowerCase().includes('text/html')
  )
    return response;
  const reader = response.body?.getReader();
  if (!reader) throw new Error('Missing HTML body');
  let size = 0;
  const chunks = [];
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.byteLength;
    if (size > 1048576) {
      await reader.cancel();
      throw new Error('HTML too large');
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.length;
  }
  const html = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  if (!/<\/head\s*>/i.test(html)) throw new Error('Missing HTML head');
  const nonce = Array.from(crypto.getRandomValues(new Uint8Array(18)), (b) =>
    b.toString(16).padStart(2, '0'),
  ).join('');
  const headers = new Headers(response.headers);
  headers.set('Content-Security-Policy', authCsp(config, nonce) + "; font-src 'self'");
  headers.delete('Content-Length');
  headers.delete('Content-Encoding');
  return new Response(html.replace(/<\/head\s*>/i, sessionTags(config, nonce, path) + '</head>'), {
    status: response.status,
    headers,
  });
}
