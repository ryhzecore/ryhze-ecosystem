/** Security applies to every response, including static files and failures. */
export function privateResponse(response, method = 'GET') {
  const headers = new Headers(response.headers);
  headers.set('Cache-Control', 'private, no-store, max-age=0');
  headers.set('CDN-Cache-Control', 'no-store');
  headers.set('Cloudflare-CDN-Cache-Control', 'no-store');
  headers.set('Pragma', 'no-cache');
  headers.set('Expires', '0');
  headers.set('X-Robots-Tag', 'noindex, nofollow, noarchive, nosnippet');
  headers.set('X-Content-Type-Options', 'nosniff');
  headers.set('Referrer-Policy', 'no-referrer');
  headers.set('X-Frame-Options', 'DENY');
  headers.set('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
  if (!headers.has('Content-Security-Policy'))
    headers.set(
      'Content-Security-Policy',
      "frame-ancestors 'none'; base-uri 'self'; form-action 'self'",
    );
  headers.set('Vary', '*');
  headers.delete('ETag');
  headers.delete('Last-Modified');
  headers.delete('Access-Control-Allow-Origin');
  return new Response(method === 'HEAD' ? null : response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

export function unavailable(method = 'GET') {
  return privateResponse(
    new Response('Private preview is not available yet.', {
      status: 503,
      headers: { 'Content-Type': 'text/plain; charset=utf-8', 'Retry-After': '3600' },
    }),
    method,
  );
}
