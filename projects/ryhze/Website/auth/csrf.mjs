// Same-origin JSON + a non-simple custom header prevents cross-origin form posts.
// Identity is independently verified from the session token for every mutation.
export function isSameOriginMutation(request, origin) {
  return (
    request.method === 'POST' &&
    request.headers.get('Origin') === origin &&
    request.headers.get('X-Ryhze-Request') === 'onboarding-v1' &&
    ['same-origin', null].includes(request.headers.get('Sec-Fetch-Site')) &&
    request.headers.get('Content-Type')?.split(';')[0].trim() === 'application/json'
  );
}
export async function readSmallJson(request, allowedKeys) {
  const length = request.headers.get('content-length');
  if (length && (!/^\d+$/.test(length) || Number(length) > 2048)) return null;
  const reader = request.body?.getReader();
  if (!reader) return null;
  const chunks = [];
  let size = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    size += value.byteLength;
    if (size > 2048) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  try {
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    const body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    if (
      !body ||
      Array.isArray(body) ||
      typeof body !== 'object' ||
      Object.keys(body).some((key) => !allowedKeys.includes(key))
    )
      return null;
    return body;
  } catch {
    return null;
  }
}
