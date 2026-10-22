import { initializeNavigation } from './navigation';
import { initializeSkipLink } from './skip-link';
import { initializeEntertainment } from './entertainment';
import { initializePlatformFrames } from './platform-frames';
import { initializeSearch } from './entertainment-search';

// One client startup. Entertainment installs category restoration before its modals.
initializeNavigation(document, window);
initializeSkipLink(document, window);
if (document.querySelector('[data-entertainment-root]')) initializeEntertainment(document, window);
else initializePlatformFrames(document, window);
initializeSearch(document);
