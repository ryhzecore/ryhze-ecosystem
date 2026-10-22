import { frameMotion } from './frame-motion';

/** Enhance native disclosures without replacing their HTML semantics. */
export function initializeNavigation(doc: Document, win: Window): () => void {
  const controller = new AbortController();
  const { signal } = controller;
  const reducedMotion = win.matchMedia('(prefers-reduced-motion: reduce)');
  type DropdownState = {
    element: HTMLDetailsElement;
    trigger: HTMLElement;
    frame: HTMLElement;
    items: HTMLAnchorElement[];
    expanded: boolean;
    animation: Animation | null;
  };
  const dropdowns: DropdownState[] = [];
  for (const element of doc.querySelectorAll<HTMLDetailsElement>('[data-dropdown]')) {
    const trigger = element.querySelector<HTMLElement>('summary');
    const frame = element.querySelector<HTMLElement>('.dropdown-frame');
    const items = [...element.querySelectorAll<HTMLAnchorElement>('.dropdown-frame a')];
    if (!trigger || !frame) continue;
    dropdowns.push({ element, trigger, frame, items, expanded: element.open, animation: null });
    element.setAttribute('data-enhanced', '');
    trigger.setAttribute('aria-expanded', String(element.open));
  }

  const cancelAnimation = (state: DropdownState): void => {
    if (!state.animation) return;
    state.animation.onfinish = null;
    state.animation.cancel();
    state.animation = null;
  };
  const setDropdown = (state: DropdownState, expanded: boolean, immediate = false): void => {
    if (state.expanded === expanded && !immediate) return;
    const { element, trigger, frame } = state;
    const wasVisible = state.expanded || state.animation !== null;
    // Read the current painted position before cancelling so interrupted motion
    // continues from that position instead of jumping to either endpoint.
    const computed =
      typeof win.getComputedStyle === 'function' ? win.getComputedStyle(frame) : null;
    const start = wasVisible
      ? { opacity: computed?.opacity || '1', transform: computed?.transform || 'none' }
      : {
          opacity: '0',
          transform: `translateY(${computed?.getPropertyValue('--dropdown-offset').trim() || '-8px'})`,
        };
    cancelAnimation(state);
    state.expanded = expanded;
    trigger.setAttribute('aria-expanded', String(expanded));
    element.toggleAttribute('data-closing', !expanded);
    if (expanded) {
      element.open = true;
      frame.removeAttribute('inert');
      frame.removeAttribute('aria-hidden');
      frame.querySelector<HTMLInputElement>('[data-search-input]')?.focus();
    } else {
      // A fading panel is already dismissed semantically and cannot be tabbed
      // into or activated. Never leave focus inside an inert/hidden subtree.
      if (frame.contains(doc.activeElement)) trigger.focus();
      frame.setAttribute('inert', '');
      frame.setAttribute('aria-hidden', 'true');
    }
    const finish = (): void => {
      cancelAnimation(state);
      if (!state.expanded) element.open = false;
      element.removeAttribute('data-closing');
    };
    const motion = frameMotion(computed, expanded);
    if (
      immediate ||
      reducedMotion.matches ||
      motion.duration === 0 ||
      typeof frame.animate !== 'function'
    ) {
      finish();
      return;
    }
    const end = expanded
      ? { opacity: '1', transform: 'translateY(0)' }
      : {
          opacity: '0',
          transform: `translateY(${computed?.getPropertyValue('--dropdown-offset').trim() || '-8px'})`,
        };
    const animation = frame.animate([start, end], motion);
    state.animation = animation;
    animation.onfinish = () => {
      if (state.animation === animation) finish();
    };
  };
  const closeDropdowns = (except?: DropdownState, immediate = false): void => {
    for (const state of dropdowns) if (state !== except) setDropdown(state, false, immediate);
  };

  for (const state of dropdowns) {
    const { element, trigger, items } = state;
    trigger.addEventListener(
      'click',
      (event: MouseEvent) => {
        // Keyboard Enter/Space still produce the native summary click. Only
        // delay native closing long enough to show the exit animation.
        event.preventDefault();
        const expanded = !state.expanded;
        if (expanded) closeDropdowns(state);
        setDropdown(state, expanded);
      },
      { signal },
    );
    element.addEventListener(
      'toggle',
      () => {
        // Also reconcile browser- or script-initiated changes to details.open.
        // Our own delayed close must not be mistaken for another open request.
        if (element.hasAttribute('data-closing') || element.open === state.expanded) return;
        if (element.open) closeDropdowns(state);
        setDropdown(state, element.open);
      },
      { signal },
    );
    element.addEventListener(
      'keydown',
      (event: KeyboardEvent) => {
        if (event.key === 'Escape' && state.expanded) {
          setDropdown(state, false);
          trigger.focus();
          event.preventDefault();
        }
        if (items.length && ['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
          if ((event.target as Element | null)?.closest('input, textarea')) return;
          event.preventDefault();
          closeDropdowns(state);
          setDropdown(state, true);
          const current = items.indexOf(doc.activeElement as HTMLAnchorElement);
          const index =
            event.key === 'Home'
              ? 0
              : event.key === 'End'
                ? items.length - 1
                : event.key === 'ArrowDown'
                  ? (current + 1) % items.length
                  : current < 0
                    ? items.length - 1
                    : (current - 1 + items.length) % items.length;
          items[index]?.focus();
        }
      },
      { signal },
    );
    element.addEventListener(
      'focusout',
      () => {
        win.requestAnimationFrame(() => {
          if (!signal.aborted && !element.contains(doc.activeElement)) setDropdown(state, false);
        });
      },
      { signal },
    );
    element.addEventListener(
      'click',
      (event: MouseEvent) => {
        if ((event.target as Element | null)?.closest('.dropdown-frame a, [data-close-dropdown]'))
          setDropdown(state, false);
      },
      { signal },
    );
  }

  doc.addEventListener(
    'click',
    (event: MouseEvent) => {
      const target = event.target as Element | null;
      if (!target?.closest('[data-dropdown]')) closeDropdowns();
    },
    { signal },
  );
  const reset = (): void => {
    closeDropdowns(undefined, true);
  };
  win.addEventListener('pageshow', reset, { signal });
  win.addEventListener('popstate', reset, { signal });
  win.matchMedia('(min-width: 1025px)').addEventListener('change', reset, { signal });
  reducedMotion.addEventListener(
    'change',
    () => {
      if (reducedMotion.matches)
        for (const state of dropdowns) setDropdown(state, state.expanded, true);
    },
    { signal },
  );
  return () => {
    controller.abort();
    for (const state of dropdowns) {
      cancelAnimation(state);
      state.element.open = state.expanded;
      state.element.removeAttribute('data-closing');
      state.element.removeAttribute('data-enhanced');
      state.trigger.removeAttribute('aria-expanded');
      state.frame.removeAttribute('inert');
      state.frame.removeAttribute('aria-hidden');
    }
  };
}
