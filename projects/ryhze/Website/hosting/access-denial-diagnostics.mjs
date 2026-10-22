/** Fixed, non-identifying denial codes. No claims, tokens, errors or URLs leave
 * the resolver. Reporting never changes its allow/deny result. */
const codes = new Set([
  'RZ-A01',
  'RZ-A02',
  'RZ-A03',
  'RZ-A04',
  'RZ-A05',
  'RZ-A06',
  'RZ-A07',
  'RZ-A08',
  'RZ-A09',
  'RZ-A10',
  'RZ-A11',
  'RZ-A12',
]);

export function reportAccessFailure(report, code, result) {
  if (codes.has(code) && typeof report === 'function') {
    try {
      report(code);
    } catch {
      /* Diagnostics must not change authorization. */
    }
  }
  return result;
}

export function accessDenialMessage(env, code, now = Date.now()) {
  const until = env?.RYHZE_DENIAL_DIAGNOSTICS_UNTIL;
  if (
    typeof until !== 'string' ||
    !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/.test(until) ||
    !codes.has(code)
  )
    return 'Access denied.';
  const expiry = Date.parse(until);
  // Default off, expires without another deployment, and cannot be enabled
  // more than 30 minutes ahead. No request parameter can enable diagnostics.
  if (!Number.isFinite(expiry) || expiry <= now || expiry - now > 30 * 60 * 1000)
    return 'Access denied.';
  return `Access denied. Reference: ${code}`;
}
