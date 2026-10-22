import pkg from '../../package.json' with { type: 'json' };
import publicPaths from './paths.json' with { type: 'json' };
import cacheManifest from './cache-manifest.json' with { type: 'json' };
const PUBLIC_PATHS = new Set(publicPaths);
const HOSTS = new Set(['ryhze.com', 'www.ryhze.com']);
// Keep retired reports out of the public presentation without redirecting
// unknown descendants or changing any backend routes.
const RETIRED_RACE_REPORTS = new Set([
  '/race/progress',
  '/race/progress/',
  '/race/progress/index.html',
  '/race/progress/scene',
  '/race/progress/scene/',
  '/race/progress/scene/index.html',
]);
// Match the build's root-level Astro CSS/JS fingerprint shape, never a prefix or
// an arbitrary filename that happens to contain a hash. Keep in sync with prepare.
const HASHED_BUNDLE =
  /^\/_astro\/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.[A-Za-z0-9_-]{8,64}\.(?:css|js|mjs)$/;
const PRIVATE_PATH =
  /(?:^|\/)(?:api|__auth|__gate|__agreements|hosting|gate|auth|portal|private|account|login|logout|oauth|session|node_modules|\.git|\.env|\.dev\.vars)(?:\/|\.|$)/i;
const IMMUTABLE_PATHS = new Set(
  cacheManifest?.schemaVersion === 1 && Array.isArray(cacheManifest.immutableAssets)
    ? cacheManifest.immutableAssets
        .filter(
          (asset) =>
            asset &&
            typeof asset.path === 'string' &&
            HASHED_BUNDLE.test(asset.path) &&
            PUBLIC_PATHS.has(asset.path) &&
            /^[a-f0-9]{64}$/.test(asset.sha256) &&
            Number.isSafeInteger(asset.bytes) &&
            asset.bytes > 0,
        )
        .map((asset) => asset.path)
    : [],
);

function cacheControl(request, response, pathname) {
  const headers = response.headers;
  const upstreamCachePolicy = [
    'Cache-Control',
    'CDN-Cache-Control',
    'Cloudflare-CDN-Cache-Control',
    'Surrogate-Control',
  ]
    .map((name) => headers.get(name) || '')
    .join(',');
  // Never turn a credentialed or personalized response into a public cache
  // entry, even if a future asset binding starts returning dynamic responses.
  if (
    [
      'Authorization',
      'Cookie',
      'CF-Access-Jwt-Assertion',
      'CF-Access-Authenticated-User-Email',
    ].some((name) => request.headers.has(name)) ||
    headers.has('Set-Cookie') ||
    headers.has('WWW-Authenticate') ||
    /(?:^|,)\s*(?:private|no-store)(?:\s*(?:=|,|$))/i.test(upstreamCachePolicy) ||
    (headers.get('Vary') || '')
      .split(',')
      .some((name) => name.trim() && name.trim().toLowerCase() !== 'accept-encoding')
  ) {
    return 'private, no-store';
  }
  // Do not persist redirects, errors, partial responses or ranged requests.
  if (
    ![200, 304].includes(response.status) ||
    request.headers.has('Range') ||
    headers.has('Content-Range')
  ) {
    return 'no-store';
  }
  const type = (headers.get('Content-Type') || '').split(';')[0].trim().toLowerCase();
  const expectedType = pathname.endsWith('.css')
    ? type === 'text/css'
    : ['text/javascript', 'application/javascript'].includes(type);
  if (IMMUTABLE_PATHS.has(pathname) && !expectedType && !(response.status === 304 && !type)) {
    return 'no-store';
  }
  // A hash-looking URL must be in the generated manifest, and a successful
  // response must be the right asset type (not an HTML fallback). 304 responses
  // may omit Content-Type; validators and conditional requests stay with ASSETS.
  if (
    IMMUTABLE_PATHS.has(pathname) &&
    (expectedType || (response.status === 304 && !type)) &&
    !/(?:^|,)\s*no-cache(?:\s*(?:=|,|$))/i.test(upstreamCachePolicy)
  ) {
    return 'public, max-age=31536000, immutable';
  }
  // HTML, metadata, unhashed fonts/images and unverified bundles can be stored,
  // but must be validated before reuse. No stale-while-revalidate or s-maxage.
  return 'public, max-age=0, must-revalidate';
}

// Only unambiguous public HTML navigations can receive the current
// not-found page. Existing routes are exact-manifest entries; everything else
// still reaches the backend first. API, account, media and asset responses stay intact.
function publicNotFoundCandidate(request, pathname) {
  return (
    ['GET', 'HEAD'].includes(request.method) &&
    !PRIVATE_PATH.test(pathname) &&
    /^\/(?:[A-Za-z0-9_-]+\/)*[A-Za-z0-9_-]*\/?$/.test(pathname) &&
    !/^\/(?:media|video|assets|images|fonts|brand|_astro)(?:\/|$)/i.test(pathname) &&
    /(?:^|,)\s*text\/html(?:\s*;|\s*,|$)/i.test(request.headers.get('Accept') || '') &&
    !['Authorization', 'CF-Access-Jwt-Assertion', 'CF-Access-Authenticated-User-Email'].some(
      (name) => request.headers.has(name),
    )
  );
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (!HOSTS.has(url.hostname)) return new Response('Not found.', { status: 404 });
    let result;
    if (!['GET', 'HEAD'].includes(request.method) || PRIVATE_PATH.test(url.pathname)) {
      return env.LEGACY.fetch(request);
    }
    if (RETIRED_RACE_REPORTS.has(url.pathname)) {
      const destination = new URL('/race/', url);
      destination.search = url.search;
      // Temporary and non-cacheable while the replacement is under review.
      // The fixed path and validated request origin prevent open redirects.
      result = new Response(null, { status: 302, headers: { Location: destination.href } });
    } else if (!PUBLIC_PATHS.has(url.pathname)) {
      const legacy = await env.LEGACY.fetch(request);
      if (
        !publicNotFoundCandidate(request, url.pathname) ||
        legacy.status !== 404 ||
        !(legacy.headers.get('Content-Type') || '').toLowerCase().startsWith('text/html') ||
        legacy.headers.has('Set-Cookie') ||
        legacy.headers.has('WWW-Authenticate')
      )
        return legacy;
      const notFound = new URL('/404.html', url);
      const page = await env.ASSETS.fetch(new Request(notFound, { method: request.method }));
      if (
        page.status !== 200 ||
        !(page.headers.get('Content-Type') || '').toLowerCase().startsWith('text/html')
      )
        return legacy;
      result = new Response(request.method === 'HEAD' ? null : page.body, {
        status: 404,
        headers: page.headers,
      });
    } else {
      result = await env.ASSETS.fetch(request);
    }
    const headers = new Headers(result.headers);
    headers.set('Cache-Control', cacheControl(request, result, url.pathname));
    // A separate CDN header must not bypass this policy in front of the Worker.
    headers.delete('CDN-Cache-Control');
    headers.delete('Cloudflare-CDN-Cache-Control');
    headers.delete('Surrogate-Control');
    headers.set('X-Content-Type-Options', 'nosniff');
    headers.set('Referrer-Policy', 'strict-origin-when-cross-origin');
    headers.set('X-Frame-Options', 'DENY');
    headers.set('X-Robots-Tag', 'noindex, nofollow, noarchive');
    headers.set('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
    headers.set('X-Ryhze-Release', pkg.version);
    return new Response(request.method === 'HEAD' ? null : result.body, {
      status: result.status,
      statusText: result.statusText,
      headers,
    });
  },
};
