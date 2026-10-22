import { initializeDetailFrame } from './detail-frame';

export function initializePlatformFrames(doc: Document, win: Window): () => void {
  const dialog = doc.querySelector<HTMLDialogElement>('[data-platform-dialog]');
  if (!dialog) return () => {};
  const views = [
    ...dialog.querySelectorAll<HTMLElement>('[data-platform-view]:not([data-download-review])'),
  ];
  const review = dialog.querySelector<HTMLElement>('[data-download-review]')!;
  const heading = dialog.querySelector<HTMLElement>('#platform-frame-heading');
  const resolve = (hash: string) =>
    views.find(
      (view) => view.dataset.platformView === hash.slice(1).replace(/^download-/, 'platform-'),
    );
  const label = (hash: string) => {
    const view = resolve(hash);
    return view
      ? `${view.dataset.frameTitle}${hash.startsWith('#download-') ? ' download' : ''}`
      : null;
  };
  const cleanup = initializeDetailFrame(doc, win, {
    kind: 'platform',
    root: 'body',
    trigger: '[data-platform-open], a[href^="/downloads/"]',
    fallback: '[data-platform-open], a[href="/downloads/"]',
    title: (hash) => (label(hash) ? `${label(hash)} — Ryhze` : null),
    request: (trigger) => {
      if (trigger.dataset.platformOpen) return `#${trigger.dataset.platformOpen}`;
      const href = trigger.getAttribute('href') || '';
      return href === '/downloads/' ? '#platforms' : `#platform-${href.split('#')[1]}`;
    },
    render: (hash) => {
      for (const view of views) view.hidden = view.dataset.platformView !== hash.slice(1);
      review.hidden = !hash.startsWith('#download-');
      if (heading) heading.textContent = label(hash);
    },
  });
  if (doc.body.dataset.platformDialogReady) {
    for (const button of doc.querySelectorAll<HTMLElement>('[data-platform-enhanced]'))
      button.hidden = false;
  }
  return () => {
    cleanup();
    for (const button of doc.querySelectorAll<HTMLElement>('[data-platform-enhanced]'))
      button.hidden = true;
  };
}
