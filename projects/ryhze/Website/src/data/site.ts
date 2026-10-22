// Public marketing content only. The product app is a separate application.
// Website version is independent of app/engine versions; availability is owner-directed.
import type { IconName } from './icons';
import pkg from '../../package.json';

export interface Discovery {
  number: string;
  brand: string;
  icon: IconName;
  title: string;
  description: string;
  action: string;
  href: string;
}
export interface Feature {
  icon: IconName;
  title: string;
  description: string;
}
export interface Platform {
  id: string;
  name: string;
  icon: IconName;
  description: string;
  note: string;
  available: false;
  status: 'Not available yet';
}
export const site = {
  name: 'Ryhze',
  version: pkg.version,
  tagline: 'ENTERTAINMENT HAS NO LIMITS',
  appUrl: '/entertainment/',
  gamesUrl: '/games/',
  filmsUrl: '/films/',
  accessNotice:
    'Downloads are not available yet for any platform. The entertainment website is still being built.',
  year: 2026,
};

export const discoveries: readonly Discovery[] = [
  {
    number: '01',
    brand: 'Films',
    icon: 'film',
    title: 'Stories that stay.',
    description: 'Discover the Films side of Ryhze as it takes shape.',
    action: 'Explore films',
    href: site.filmsUrl,
  },
  {
    number: '02',
    brand: 'Games',
    icon: 'gamepad',
    title: 'Find your own way.',
    description: 'Explore our games and follow what’s in development.',
    action: 'Explore games',
    href: site.gamesUrl,
  },
];

export const features: readonly Feature[] = [
  {
    icon: 'search',
    title: 'Explore Games and Films',
    description: 'Discover Games and Films in one place.',
  },
  {
    icon: 'screens',
    title: 'Platform availability',
    description: 'See the platforms planned for Ryhze and their current availability.',
  },
  {
    icon: 'spark',
    title: 'See what comes next',
    description: 'Follow the worlds and experiences taking shape at Ryhze.',
  },
];

// Shared unpublished-release status, not a released-app version or a legal agreement.
export const publicRelease = {
  version: null,
  date: null,
  requirements: null,
  licence: null,
  terms: null,
  privacy: null,
};

// No installer targets exist in this pre-alpha website version.
export const platforms: readonly Platform[] = [
  {
    id: 'windows',
    name: 'Windows',
    icon: 'windows',
    description: 'Ryhze for your Windows desktop.',
  },
  {
    id: 'linux',
    name: 'Linux & Steam Deck',
    icon: 'gamepad',
    description: 'Ryhze for Linux and Steam Deck.',
  },
  { id: 'android', name: 'Android', icon: 'phone', description: 'Ryhze for your Android device.' },
  { id: 'ios', name: 'iOS', icon: 'phone', description: 'Ryhze for iPhone and iPad.' },
  { id: 'macos', name: 'macOS', icon: 'laptop', description: 'Ryhze for your Mac.' },
].map((platform) => ({
  ...platform,
  icon: platform.icon as IconName,
  available: false as const,
  status: 'Not available yet' as const,
  note: 'No download or release date is available yet.',
}));

export const questions = [
  {
    question: 'What is Ryhze?',
    answer:
      'Ryhze brings together Games and Films. This website introduces our projects and the platforms being prepared for them.',
  },
  {
    question: 'Can I use Ryhze in my browser?',
    answer:
      'The entertainment website is still being built. You can explore the Games and Films showcase; playable games and films to watch are not available yet.',
  },
  {
    question: 'Can I download Ryhze?',
    answer:
      'Not yet. Downloads are unavailable for every platform in this website version, and no release date is announced.',
  },
  {
    question: 'What is the status of Linux and Steam Deck?',
    answer:
      'Linux and Steam Deck are listed among the planned platforms. A download is not available yet.',
  },
  {
    question: 'Are iOS and macOS apps available?',
    answer:
      'Not yet. Like Windows, Linux, Steam Deck and Android, iOS and macOS do not have downloads available here.',
  },
];
