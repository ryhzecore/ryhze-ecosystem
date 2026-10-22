import { initializeDetailFrame } from './detail-frame';

export function initializeTitleDialog(
  doc: Document,
  win: Window,
  syncSaved?: () => void,
): () => void {
  const root = doc.querySelector<HTMLElement>('[data-entertainment-root]');
  const dialog = root?.querySelector<HTMLDialogElement>('[data-title-dialog]');
  const views = [...(dialog?.querySelectorAll<HTMLElement>('[data-title-view]') ?? [])];
  const button = dialog?.querySelector<HTMLElement>('[data-save-title]');
  const resolve = (hash: string) =>
    views.find(
      (view) =>
        view.dataset.titleView === hash.slice(1) &&
        view.dataset.category === root!.dataset.currentCategory,
    );
  return initializeDetailFrame(doc, win, {
    kind: 'title',
    root: '[data-entertainment-root]',
    trigger: 'a[href$="-details"]',
    fallback: '.entertainment-thumbnail',
    title: (hash) => {
      const view = resolve(hash);
      return view ? `${view.dataset.titleName} — Ryhze` : null;
    },
    request: (trigger) => `#${trigger.getAttribute('href')!.split('#')[1]}`,
    render: (hash) => {
      const selected = resolve(hash);
      for (const view of views) view.hidden = view !== selected;
      if (!selected || !button) return;
      dialog!.setAttribute('aria-labelledby', selected.querySelector('h2')!.id);
      button.dataset.saveTitle = selected.dataset.titleId;
      button.dataset.saveName = selected.dataset.titleName;
      syncSaved?.();
    },
  });
}
