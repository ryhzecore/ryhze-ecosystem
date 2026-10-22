/** Share pointer/keyboard focus intent and keep the skip link keyboard-only. */
export function initializeSkipLink(doc: Document, win: Window): () => void {
  const link = doc.querySelector<HTMLAnchorElement>('.skip-link');
  if (!link) return () => {};
  const controller = new AbortController();
  const { signal } = controller;
  const root = doc.documentElement;
  root.dataset.pointer = '';
  let keyboardIntent = false;
  link.setAttribute('data-skip-enhanced', '');

  const hide = (blur = false): void => {
    keyboardIntent = false;
    link.removeAttribute('data-keyboard-focus');
    if (blur && doc.activeElement === link) link.blur();
  };
  doc.addEventListener(
    'keydown',
    (event: KeyboardEvent) => {
      if (event.altKey || event.ctrlKey || event.metaKey) return;
      delete root.dataset.pointer;
      if (event.key === 'Tab') {
        keyboardIntent = true;
        if (doc.activeElement === link) link.setAttribute('data-keyboard-focus', '');
      } else if (event.key === 'Escape') hide(true);
    },
    { signal, capture: true },
  );
  // A focus event consumes the one Tab transition; keyup also clears intent if
  // focus went to browser chrome. Neither event changes persistent input mode.
  const clearIntent = () => (keyboardIntent = false);
  for (const type of ['keyup', 'focusin']) doc.addEventListener(type, clearIntent, { signal });
  link.addEventListener(
    'focus',
    () => {
      link.toggleAttribute('data-keyboard-focus', keyboardIntent);
    },
    { signal },
  );
  link.addEventListener('blur', () => hide(), { signal });
  const clearPointerFocus = (event: Event): void => {
    root.dataset.pointer = '';
    // A user may click a link they first revealed with the keyboard.
    if (event.target === link && link.hasAttribute('data-keyboard-focus')) return;
    hide(true);
  };
  for (const type of ['pointerdown', 'mousedown', 'touchstart']) {
    doc.addEventListener(type, clearPointerFocus, { signal, capture: true, passive: true });
  }
  doc.addEventListener('wheel', () => hide(true), { signal, passive: true });
  win.addEventListener('pageshow', () => hide(true), { signal });
  link.addEventListener(
    'click',
    () => {
      // The native anchor still updates the fragment and scrolls to #main.
      // tabindex=-1 lets this hand off keyboard focus without a new tab stop.
      doc.getElementById('main')?.focus({ preventScroll: true });
      hide();
    },
    { signal },
  );
  return () => {
    controller.abort();
    delete root.dataset.pointer;
    hide();
    link.removeAttribute('data-skip-enhanced');
  };
}
