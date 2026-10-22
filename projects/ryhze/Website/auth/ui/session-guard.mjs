// Runs on every authorized HTML document, including the legal-review pages.
// Clerk's standard-browser SDK maintains its session cookie; explicit getToken
// refreshes it before each server-authoritative access check and on resume.
export async function bootSessionGuard(config) {
  let busy = false;
  let stopped = false;
  let timer;
  let clerk;
  const returnTo = config.returnTo;
  const close = (message) => {
    stopped = true;
    if (timer) clearInterval(timer);
    const main = document.createElement('main');
    const heading = document.createElement('h1');
    heading.textContent = 'Private preview locked';
    const detail = document.createElement('p');
    detail.textContent = message;
    const link = document.createElement('a');
    link.textContent = 'Return to secure sign-in';
    link.href = '/__auth/sign-in?returnTo=' + encodeURIComponent(returnTo);
    main.append(heading, detail, link);
    document.body.replaceChildren(main);
  };
  const check = async () => {
    if (busy || stopped || document.visibilityState === 'hidden') return;
    busy = true;
    try {
      if (!clerk.session) {
        close('Your session ended. Sign in again to continue.');
        return;
      }
      const token = await clerk.session.getToken({ skipCache: true });
      if (!token) {
        close('Your session ended. Sign in again to continue.');
        return;
      }
      const response = await fetch(
        config.reviewingAgreements ? '/__auth/session' : '/__auth/content-status',
        {
          headers: { Authorization: `Bearer ${token}` },
          credentials: 'same-origin',
          redirect: 'manual',
          cache: 'no-store',
        },
      );
      if (response.status !== (config.reviewingAgreements ? 200 : 204))
        close(
          'Access could not be confirmed. Your invitation, session or agreements may need attention.',
        );
    } catch {
      close(
        'Secure access is temporarily unavailable. Sign in again when the connection is restored.',
      );
    } finally {
      busy = false;
    }
  };
  try {
    clerk = window.Clerk;
    if (!clerk) throw new Error('SDK unavailable');
    await clerk.load({
      nonce: config.nonce,
      telemetry: { disabled: true },
      signInUrl: config.origin + '/__auth/sign-in',
      signUpUrl: config.origin + '/__auth/sign-up',
    });
    await check();
    if (stopped) return;
    timer = setInterval(check, 30000);
    window.addEventListener('focus', check);
    window.addEventListener('pageshow', check);
    document.addEventListener('visibilitychange', check);
    clerk.addListener?.(({ session }) => {
      if (!session) close('Your session ended. Sign in again to continue.');
    });
  } catch {
    close('Secure session refresh could not start. Return to sign-in to try again.');
  }
}
