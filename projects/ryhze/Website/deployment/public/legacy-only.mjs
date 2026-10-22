const HOSTS = new Set(['ryhze.com', 'www.ryhze.com']);
export default {
  async fetch(request, env) {
    if (!HOSTS.has(new URL(request.url).hostname))
      return new Response('Not found.', { status: 404 });
    return env.LEGACY.fetch(request);
  },
};
