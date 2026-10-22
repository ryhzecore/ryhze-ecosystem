import type { APIRoute } from 'astro';
import pkg from '../../package.json';

// Public, non-secret release metadata only. No repository tokens or source paths.
export const GET: APIRoute = () =>
  new Response(
    JSON.stringify({
      product: 'ryhze-website',
      version: pkg.version,
      channel: 'development',
      stage: 'PRE-ALPHA',
    }),
    { headers: { 'Content-Type': 'application/json' } },
  );
