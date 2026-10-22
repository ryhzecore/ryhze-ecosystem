import { initializeCategoryDrag } from './category-drag';
import { initializeTitleDialog } from './title-dialog';
import { closeDetailFrame } from './detail-frame';
import { initializePlatformFrames } from './platform-frames';

/** Keep ordinary category changes local while native links remain usable without JS. */
export function initializeEntertainment(doc: Document, win: Window): () => void {
  const root = doc.querySelector<HTMLElement>('[data-entertainment-root]');
  const content = root?.querySelector<HTMLElement>('[data-entertainment-content]');
  if (!root || !content) return () => {};
  const stopCategoryDrag = initializeCategoryDrag(doc);
  const controller = new AbortController();
  const { signal } = controller;
  const announcement = root.querySelector<HTMLElement>('[data-entertainment-announcement]');
  const chooser = doc.querySelector<HTMLElement>('[data-entertainment-chooser]')!;
  const links = [...chooser.querySelectorAll<HTMLAnchorElement>('[data-category-link]')];
  root.querySelector<HTMLElement>('[data-favourites-note]')!.hidden = false;
  const saved = new Set<string>(); // This document only. No account, storage, or tracking request.
  const syncSaved = () => {
    for (const controls of content.querySelectorAll<HTMLElement>('[data-exclusive-controls]'))
      controls.hidden = false;
    for (const button of root.querySelectorAll<HTMLElement>('[data-save-title]')) {
      const active = saved.has(button.dataset.saveTitle!);
      button.hidden = false;
      button.setAttribute('aria-pressed', String(active));
      button.setAttribute(
        'aria-label',
        active
          ? `Remove ${button.dataset.saveName} from favourites`
          : `Save ${button.dataset.saveName} temporarily`,
      );
      button.querySelector('[data-save-label]')!.textContent = active
        ? 'Saved temporarily'
        : 'Favourites';
    }
  };
  syncSaved();
  const selectCategory = (category: string) => {
    if (root.dataset.currentCategory === category) return;
    const template = root.querySelector<HTMLTemplateElement>(
      `[data-category-template="${category}"]`,
    );
    if (!template) return;
    const moveFocus = content.contains(doc.activeElement);
    content.replaceChildren(template.content.cloneNode(true));
    root.dataset.currentCategory = chooser.dataset.category = category;
    for (const link of links) {
      if (link.dataset.categoryLink === category) {
        link.setAttribute('aria-current', 'page');
        if (moveFocus) link.focus({ preventScroll: true });
      } else link.removeAttribute('aria-current');
    }
    doc.title = template.dataset.title!;
    syncSaved();
    if (announcement)
      announcement.textContent = `${category === 'games' ? 'Games' : 'Films'} selected`;
  };
  const ordinaryClick = (event: MouseEvent) =>
    !event.defaultPrevented &&
    !event.button &&
    !event.metaKey &&
    !event.ctrlKey &&
    !event.shiftKey &&
    !event.altKey;
  // Select a search result's category before the shared title-frame listener opens it.
  doc.addEventListener(
    'click',
    (event: MouseEvent) => {
      const result = (event.target as Element | null)?.closest('.search-result');
      const category = result?.getAttribute('href')?.split('/')[1];
      if (category && ordinaryClick(event) && category !== root.dataset.currentCategory) {
        win.history.pushState(null, '', `/${category}/`);
        selectCategory(category);
      }
    },
    { signal, capture: true },
  );
  win.addEventListener(
    'popstate',
    () => selectCategory(win.location.pathname === '/films/' ? 'films' : 'games'),
    { signal },
  );
  doc.addEventListener(
    'click',
    (event: MouseEvent) => {
      const target = event.target as Element | null;
      const link = target?.closest<HTMLAnchorElement>('a');
      const category = link?.getAttribute('href')?.match(/^\/(games|films)\/$/)?.[1];
      if (category && ordinaryClick(event) && !link?.target && !link?.hasAttribute('download')) {
        event.preventDefault();
        if (category !== root.dataset.currentCategory || win.location.hash) {
          closeDetailFrame(doc);
          win.history.pushState(null, '', `/${category}/`);
          selectCategory(category);
        }
        return;
      }
      if (!target || !root.contains(target) || target.closest('[inert]')) return;
      if (
        root.dataset.titleDialogReady !== 'true' &&
        link?.getAttribute('href')?.endsWith('-details')
      ) {
        const details = content.querySelector<HTMLDetailsElement>(
          `#${link.getAttribute('href')!.split('#')[1]}`,
        );
        if (details) details.open = true;
      }
      const step = target.closest<HTMLElement>('[data-exclusive-step]');
      const showcase = step?.closest('[data-exclusive-showcase]');
      if (step && showcase) {
        const slides = [...showcase.querySelectorAll<HTMLElement>('[data-exclusive-slide]')];
        const current = slides.findIndex((slide) => !slide.hidden);
        const next = (current + Number(step.dataset.exclusiveStep) + slides.length) % slides.length;
        slides.forEach((slide, index) => {
          slide.hidden = index !== next;
        });
        if (announcement)
          announcement.textContent = `Ryhze exclusive ${next + 1} of ${slides.length}`;
        return;
      }
      const button = target?.closest<HTMLElement>('[data-save-title]');
      if (!button) return;
      const id = button.dataset.saveTitle!;
      if (!saved.delete(id)) saved.add(id);
      syncSaved();
      if (announcement)
        announcement.textContent = saved.has(id)
          ? `${button.dataset.saveName} saved temporarily`
          : `${button.dataset.saveName} removed from favourites`;
    },
    { signal },
  );
  const restoreDetails = () => {
    if (
      root.dataset.titleDialogReady !== 'true' &&
      ['#game-details', '#film-details'].includes(win.location.hash)
    ) {
      const details = content.querySelector<HTMLDetailsElement>(win.location.hash);
      if (details) details.open = true;
    }
  };
  restoreDetails();
  win.addEventListener('hashchange', restoreDetails, { signal });
  win.addEventListener('pageshow', restoreDetails, { signal });
  // Reset before a document enters the back/forward cache as well, so the visible
  // promise is consistent whether history restores or reloads the old document.
  win.addEventListener(
    'pagehide',
    () => {
      saved.clear();
      syncSaved();
      if (announcement) announcement.textContent = '';
    },
    { signal },
  );
  // Both modal restorers must run after category content and its title are restored.
  const stopPlatformFrames = initializePlatformFrames(doc, win);
  const stopTitleDialog = initializeTitleDialog(doc, win, syncSaved);
  return () => {
    stopTitleDialog();
    stopPlatformFrames();
    stopCategoryDrag();
    controller.abort();
  };
}
