// Local line icons; no runtime icon library or external requests.
// Films and Games preserve the original public category symbols in new SVG geometry.
export const iconPaths = {
  arrow: '<path d="M4 12h15M13 6l6 6-6 6"/>',
  down: '<path d="M12 3v12m-5-5 5 5 5-5M5 16v4h14v-4"/>',
  external: '<path d="M14 4h6v6m-1-5-9 9M10 4H4v16h16v-6"/>',
  film: '<rect x="3" y="4" width="18" height="16"/><path d="M7 4V20M17 4V20M3 9H7M3 15H7M17 9H21M17 15H21"/>',
  play: '<path d="m7 3 14 9-14 9z"/>',
  gamepad:
    '<path d="M7 7H17C21 7 23 18 20 19C18 20 16 16 15 16H9C8 16 6 20 4 19C1 18 3 7 7 7Z"/><path d="M6 10V15M4 13H8M15 11H15.1M18 14H18.1"/>',
  search: '<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/>',
  screens: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 21h8m-4-5v5"/>',
  spark: '<path d="m12 3 2.5 6.5L21 12l-6.5 2.5L12 21l-2.5-6.5L3 12l6.5-2.5z"/>',
  windows: '<path d="m3 5 8-1v7H3Zm10-1 8-1v8h-8ZM3 13h8v7l-8-1Zm10 0h8v8l-8-1Z"/>',
  phone: '<rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 5h4m-3 14h2"/>',
  laptop: '<rect x="5" y="4" width="14" height="12" rx="1"/><path d="m5 16-3 4h20l-3-4"/>',
  globe:
    '<circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="4" ry="9"/><path d="M3 12h18"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  close: '<path d="m6 6 12 12M6 18 18 6"/>',
} as const;

export type IconName = keyof typeof iconPaths;
