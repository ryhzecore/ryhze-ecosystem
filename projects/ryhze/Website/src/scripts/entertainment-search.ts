/** Filter the actual public catalogue locally; no query is sent or persisted. */
export function initializeSearch(doc: Document): () => void {
  const input = doc.querySelector<HTMLInputElement>('[data-search-input]');
  if (!input) return () => {};
  const results = [...doc.querySelectorAll<HTMLElement>('[data-search-title]')];
  const empty = doc.querySelector<HTMLElement>('[data-search-empty]');
  const update = () => {
    const query = input.value.trim().toLowerCase();
    let matches = 0;
    for (const result of results) {
      result.hidden = !result.dataset.searchTitle?.includes(query);
      if (!result.hidden) matches++;
    }
    if (empty) empty.hidden = matches > 0;
  };
  input.addEventListener('input', update);
  update();
  return () => input.removeEventListener('input', update);
}
