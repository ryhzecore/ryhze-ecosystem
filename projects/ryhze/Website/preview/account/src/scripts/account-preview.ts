import { frameMotion } from '../../../../src/scripts/frame-motion';

/** A presentation-only route selector. No input listeners, fetch, SDK, storage,
 * credential handling or authentication state exist in this static specimen. */
export function bootAccountPreview(doc: Document = document, win: Window = window) {
  // Safari/WebView may focus the first link during touch navigation or restore.
  // Require an actual Tab navigation before showing the fixed skip control.
  doc.documentElement.dataset.accountInput = 'pointer';
  doc.addEventListener('keydown', (event) => {
    if (event.key === 'Tab' && !event.metaKey && !event.ctrlKey && !event.altKey)
      doc.documentElement.dataset.accountInput = 'keyboard';
  });
  const pointerInput = () => {
    doc.documentElement.dataset.accountInput = 'pointer';
  };
  doc.addEventListener('pointerdown', pointerInput, { passive: true });
  doc.addEventListener('touchstart', pointerInput, { passive: true });
  win.addEventListener('pageshow', pointerInput);
  const sections = [...doc.querySelectorAll<HTMLElement>('[data-screen]')];
  const links = [...doc.querySelectorAll<HTMLAnchorElement>('a[data-go]')];
  const names = [
    'Invitation',
    'Sign in',
    'Create account',
    'Verify email',
    'Forgot password',
    'Reset password',
    'Your profile',
    'Link unavailable',
  ];
  const reviewOrder = [
    'invitation',
    'sign-up',
    'verify-email',
    'profile',
    'sign-in',
    'forgot-password',
    'reset-password',
    'unavailable',
  ];
  const ids = sections.map((section) => section.dataset.screen!);
  const card = doc.getElementById('account-card');
  const counter = doc.getElementById('screen-counter');
  const next = doc.getElementById('next-screen') as HTMLAnchorElement | null;
  const status = doc.getElementById('preview-status');
  let animation: Animation | null = null;
  const render = (screen: string, focus: boolean) => {
    const index = ids.indexOf(screen);
    if (index < 0) return;
    animation?.cancel();
    sections.forEach((section) => {
      section.hidden = section.dataset.screen !== screen;
    });
    links.forEach((link) => {
      if (link.closest('.screen-picker') && link.dataset.go === screen)
        link.setAttribute('aria-current', 'page');
      else link.removeAttribute('aria-current');
    });
    doc.querySelectorAll<HTMLElement>('[data-progress]').forEach((step) => {
      const active =
        screen === 'invitation' || screen === 'unavailable'
          ? 'invitation'
          : screen === 'profile'
            ? 'profile'
            : 'account';
      step.toggleAttribute('data-active', step.dataset.progress === active);
    });
    if (counter) counter.textContent = `${String(index + 1).padStart(2, '0')} / 08`;
    const nextScreen =
      reviewOrder[(reviewOrder.indexOf(screen) + 1) % reviewOrder.length] || 'invitation';
    const nextIndex = ids.indexOf(nextScreen);
    if (next) {
      next.dataset.go = ids[nextIndex];
      next.href = '?screen=' + ids[nextIndex];
      next.textContent = `Preview ${names[nextIndex]?.toLowerCase()} →`;
    }
    doc.title = `${names[index]} · Account design preview | Ryhze`;
    if (status) status.textContent = `${names[index]} preview. Account actions remain disabled.`;
    if (focus) sections[index]?.querySelector<HTMLElement>('h2')?.focus({ preventScroll: true });
    if (focus && card?.animate && !win.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      animation = card.animate(
        [
          { opacity: 0.55, transform: 'translateY(6px)' },
          { opacity: 1, transform: 'translateY(0)' },
        ],
        frameMotion(win.getComputedStyle(card), true),
      );
    }
  };
  const fromUrl = () => {
    const screen = new URL(win.location.href).searchParams.get('screen');
    return screen && ids.includes(screen) ? screen : 'invitation';
  };
  for (const link of links)
    link.addEventListener('click', (event) => {
      if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey)
        return;
      const screen = link.dataset.go;
      if (!screen || !ids.includes(screen)) return;
      event.preventDefault();
      // Deliberately construct a new allowlisted URL. Never carry invitation tokens.
      win.history.pushState(null, '', win.location.pathname + '?screen=' + screen);
      render(screen, true);
    });
  win.addEventListener('popstate', () => render(fromUrl(), true));
  const first = fromUrl();
  // Strip unknown query/hash values, including accidentally pasted live invite links.
  win.history.replaceState(
    null,
    '',
    win.location.pathname + (first === 'invitation' ? '' : '?screen=' + first),
  );
  render(first, false);
}

if (typeof document !== 'undefined') bootAccountPreview();
