import { frameMotion } from './frame-motion';

const activeFrame = Symbol.for('ryhze.detail-frame');
type FrameDocument = Document & { [activeFrame]?: () => void };

/** Release the active modal when navigation changes the document's main content. */
export function closeDetailFrame(doc: FrameDocument): void {
  doc[activeFrame]?.();
}

interface DetailFrameOptions {
  kind: 'title' | 'platform';
  root: string;
  trigger: string;
  fallback: string;
  title: (hash: string) => string | null;
  request: (trigger: HTMLElement) => string | null;
  render?: (hash: string) => void;
}

/** Shared native detail frame: modal focus, token-based motion and reversible history. */
export function initializeDetailFrame(
  doc: FrameDocument,
  win: Window,
  options: DetailFrameOptions,
): () => void {
  const selector = (part: string) => `[data-${options.kind}-${part}]`;
  const close = `[data-close-${options.kind}]`;
  const ready = `${options.kind}DialogReady`;
  const dialog = doc.querySelector<HTMLDialogElement>(selector('dialog'));
  const panel = dialog?.querySelector<HTMLElement>(selector('panel'));
  const root = doc.querySelector<HTMLElement>(options.root);
  if (!dialog || !panel || !root || typeof dialog.showModal !== 'function') return () => {};
  const controller = new AbortController();
  const { signal } = controller;
  const media = win.matchMedia('(prefers-reduced-motion: reduce)');
  let opener: HTMLElement | null = null;
  let originalTitle = doc.title;
  let currentHash = '';
  let frameTitle = '';
  let overflow = '';
  let animation: Animation | null = null;
  let desiredOpen = false;
  let ownsHistory = false;
  let returning = false;
  let focusFrame = 0;
  const restoreFocus = () => {
    if (
      desiredOpen &&
      dialog.open &&
      (doc.activeElement === dialog || !dialog.contains(doc.activeElement))
    )
      dialog.querySelector<HTMLButtonElement>(close)?.focus({ preventScroll: true });
  };
  root.dataset[ready] = 'true';
  const cancelMotion = () => {
    if (animation) {
      animation.onfinish = null;
      animation.cancel();
    }
    animation = null;
  };
  const finishClosed = () => {
    if (doc[activeFrame] === release) delete doc[activeFrame];
    cancelMotion();
    if (dialog.open) dialog.close();
    panel.removeAttribute('inert');
    panel.removeAttribute('aria-hidden');
    dialog.dataset.state = 'closed';
    doc.body.style.overflow = overflow;
    if (doc.title === frameTitle) doc.title = originalTitle;
    if (opener?.isConnected && !opener.closest('[inert]')) opener.focus({ preventScroll: true });
  };
  const release = () => {
    desiredOpen = false;
    finishClosed();
  };
  const animate = (opening: boolean) => {
    const computed = win.getComputedStyle(dialog);
    const from = dialog.open && animation ? computed.opacity : opening ? '0' : '1';
    cancelMotion();
    dialog.dataset.state = opening ? 'opening' : 'closing';
    const motion = frameMotion(computed, opening);
    if (media.matches || motion.duration === 0 || typeof dialog.animate !== 'function') {
      if (opening) dialog.dataset.state = 'open';
      else finishClosed();
      return;
    }
    const running = dialog.animate([{ opacity: from }, { opacity: opening ? 1 : 0 }], motion);
    animation = running;
    running.onfinish = () => {
      if (animation !== running || signal.aborted) return;
      if (desiredOpen) {
        cancelMotion();
        dialog.dataset.state = 'open';
      } else finishClosed();
    };
  };
  const show = (hash: string, trigger: HTMLElement | null = null) => {
    if (desiredOpen && currentHash === hash) return;
    if (doc[activeFrame] !== release) doc[activeFrame]?.();
    doc[activeFrame] = release;
    currentHash = hash;
    frameTitle = options.title(hash) || originalTitle;
    options.render?.(hash);
    desiredOpen = true;
    panel.removeAttribute('inert');
    panel.removeAttribute('aria-hidden');
    if (!dialog.open) {
      opener =
        trigger?.closest('[data-dropdown]')?.querySelector<HTMLElement>('summary') ??
        trigger ??
        (opener?.isConnected ? opener : doc.querySelector<HTMLElement>(options.fallback));
      originalTitle = doc.title;
      overflow = doc.body.style.overflow;
      doc.body.style.overflow = 'hidden';
      dialog.showModal();
      const scroll = dialog.querySelector<HTMLElement>(selector('scroll'));
      if (scroll) scroll.scrollTop = 0;
    }
    if (options.render) {
      const scroll = dialog.querySelector<HTMLElement>(selector('scroll'));
      if (scroll) scroll.scrollTop = 0;
      dialog.querySelector<HTMLButtonElement>(close)?.focus({ preventScroll: true });
    }
    doc.title = frameTitle;
    animate(true);
    restoreFocus();
  };
  const hide = () => {
    if (!dialog.open || !desiredOpen) return;
    desiredOpen = false;
    // Keep native modal blocking through the fade, but move focus out of the
    // dismissed controls before making them inert and hidden to assistive tech.
    if (panel.contains(doc.activeElement)) dialog.focus({ preventScroll: true });
    panel.setAttribute('inert', '');
    panel.setAttribute('aria-hidden', 'true');
    animate(false);
  };
  const dismiss = () => {
    if (!desiredOpen || returning) return;
    // History traversal is asynchronous: dismissal must take effect now.
    hide();
    if (ownsHistory && options.title(win.location.hash) !== null) {
      returning = true;
      win.history.back();
    } else {
      if (options.title(win.location.hash) !== null)
        win.history.replaceState(
          win.history.state,
          '',
          `${win.location.pathname}${win.location.search}`,
        );
    }
  };
  const restore = () => {
    returning = false;
    if (options.title(win.location.hash)) {
      ownsHistory = win.history.state?.ryhzeDetailFrame === true;
      show(win.location.hash);
      restoreFocus();
      win.cancelAnimationFrame(focusFrame);
      focusFrame = win.requestAnimationFrame(restoreFocus);
    } else {
      ownsHistory = false;
      hide();
    }
  };
  doc.addEventListener(
    'click',
    (event: MouseEvent) => {
      const trigger = (event.target as Element | null)?.closest<HTMLAnchorElement>(options.trigger);
      if (
        !trigger ||
        event.defaultPrevented ||
        event.button !== 0 ||
        event.metaKey ||
        event.ctrlKey ||
        event.shiftKey ||
        event.altKey
      )
        return;
      const hash = options.request(trigger);
      if (!hash || !options.title(hash)) return;
      event.preventDefault();
      if (returning) return;
      if (win.location.hash !== hash) {
        win.history.pushState(
          { ...win.history.state, ryhzeDetailFrame: true },
          '',
          `${win.location.pathname}${win.location.search}${hash}`,
        );
        ownsHistory = true;
      }
      show(hash, trigger);
    },
    { signal },
  );
  dialog.addEventListener(
    'click',
    (event) => {
      if (!desiredOpen) {
        event.preventDefault();
        event.stopImmediatePropagation();
        return;
      }
      if (event.target === dialog || (event.target as Element | null)?.closest(close)) dismiss();
    },
    { signal, capture: true },
  );
  dialog.addEventListener(
    'keydown',
    (event) => {
      if (!desiredOpen) {
        event.preventDefault();
        event.stopImmediatePropagation();
      }
    },
    { signal, capture: true },
  );
  dialog.addEventListener(
    'cancel',
    (event) => {
      event.preventDefault();
      dismiss();
    },
    { signal },
  );
  win.addEventListener('popstate', restore, { signal });
  win.addEventListener('hashchange', restore, { signal });
  win.addEventListener('pageshow', restore, { signal });
  media.addEventListener(
    'change',
    () => {
      if (!media.matches || !animation) return;
      cancelMotion();
      if (desiredOpen) dialog.dataset.state = 'open';
      else finishClosed();
    },
    { signal },
  );
  restore();
  return () => {
    controller.abort();
    win.cancelAnimationFrame(focusFrame);
    desiredOpen = false;
    if (dialog.open) finishClosed();
    else cancelMotion();
    delete root.dataset[ready];
  };
}
