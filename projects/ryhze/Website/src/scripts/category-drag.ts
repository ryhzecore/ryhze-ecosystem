/** Pointer sliding activates the same links used by clicks and keyboard navigation. */
export function initializeCategoryDrag(doc: Document): () => void {
  const chooser = doc.querySelector<HTMLElement>('[data-entertainment-chooser]');
  if (!chooser) return () => {};
  const links = chooser.querySelectorAll<HTMLAnchorElement>('[data-category-link]');
  const pill = chooser.querySelector<HTMLElement>('.entertainment-selection');
  if (links.length !== 2 || !pill) return () => {};
  const controller = new AbortController();
  const options = { signal: controller.signal };
  let pointer: number | null = null;
  let x = 0,
    y = 0,
    distance = 0,
    start = 0,
    position = 0;
  let active = false,
    suppressClick = false;
  const finish = (commit = false) => {
    if (pointer === null) return;
    const id = pointer;
    pointer = null;
    if (chooser.hasPointerCapture?.(id)) chooser.releasePointerCapture(id);
    delete chooser.dataset.dragging;
    pill.style.removeProperty('transform');
    const next = position >= 0.5 ? 1 : 0;
    if (commit && active && next !== start) links[next]!.click();
    active = false;
  };
  chooser.addEventListener(
    'pointerdown',
    (event) => {
      if (event.button || event.isPrimary === false || pointer !== null) return;
      distance = links[1]!.getBoundingClientRect().left - links[0]!.getBoundingClientRect().left;
      if (!distance) return;
      pointer = event.pointerId;
      x = event.clientX;
      y = event.clientY;
      start = position = chooser.dataset.category === 'games' ? 0 : 1;
      suppressClick = false;
    },
    options,
  );
  chooser.addEventListener(
    'pointermove',
    (event) => {
      if (event.pointerId !== pointer) return;
      const dx = event.clientX - x,
        dy = event.clientY - y;
      if (!active) {
        if (Math.abs(dy) > Math.max(6, Math.abs(dx))) {
          finish();
          return;
        }
        if (Math.abs(dx) < 6) return;
        active = suppressClick = true;
        chooser.setPointerCapture?.(pointer);
        chooser.dataset.dragging = 'true';
      }
      event.preventDefault();
      position = Math.max(0, Math.min(1, start + dx / distance));
      pill.style.transform = `translateX(${position * distance}px)`;
    },
    options,
  );
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    chooser.addEventListener(
      type,
      (event) => {
        if ((event as PointerEvent).pointerId === pointer) finish(type === 'pointerup');
      },
      options,
    );
  }
  chooser.addEventListener(
    'click',
    (event) => {
      if (suppressClick && event.detail !== 0) {
        event.preventDefault();
        suppressClick = false;
      }
    },
    options,
  );
  return () => {
    finish();
    controller.abort();
  };
}
