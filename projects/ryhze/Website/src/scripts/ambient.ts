/** Ambient page glows move gently upward with scroll, never autonomously. */
export function initializeAmbient(doc: Document, win: Window): () => void {
  const glows = [...doc.querySelectorAll<HTMLElement>('[data-ambient-glow]')];
  if (!glows.length) return () => {};
  const reducedMotion = win.matchMedia('(prefers-reduced-motion: reduce)');
  let frame: number | null = null;
  let disposed = false;
  const cancel = (): void => {
    if (frame !== null) win.cancelAnimationFrame(frame);
    frame = null;
  };
  const update = (): void => {
    frame = null;
    if (disposed || reducedMotion.matches) return;
    const scroll = Math.max(0, win.scrollY || 0);
    for (const glow of glows) {
      const rate = glow.dataset.ambientGlow === 'top' ? 0.075 : 0.11;
      const offset = Math.min(scroll * rate, 240);
      glow.style.transform = `translate3d(0, ${-offset}px, 0)`;
    }
  };
  const schedule = (): void => {
    if (!disposed && !reducedMotion.matches && frame === null)
      frame = win.requestAnimationFrame(update);
  };
  const preferenceChanged = (): void => {
    cancel();
    if (reducedMotion.matches) {
      for (const glow of glows) glow.style.removeProperty('transform');
    } else {
      schedule();
    }
  };
  win.addEventListener('scroll', schedule, { passive: true });
  win.addEventListener('pageshow', schedule);
  reducedMotion.addEventListener('change', preferenceChanged);
  schedule();
  return () => {
    disposed = true;
    cancel();
    win.removeEventListener('scroll', schedule);
    win.removeEventListener('pageshow', schedule);
    reducedMotion.removeEventListener('change', preferenceChanged);
    for (const glow of glows) glow.style.removeProperty('transform');
  };
}
