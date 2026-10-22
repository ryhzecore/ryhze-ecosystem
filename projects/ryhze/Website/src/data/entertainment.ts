export interface ExclusiveTitle {
  id: string;
  title: string;
  titleLines: readonly string[];
  category: 'games' | 'films';
  description: string;
  compactDescription: string;
  summary: string;
  mediaLabel: string;
  availability: string;
  genres?: readonly string[];
  setting?: string;
  status: string;
  genre?: string;
  href: string;
}

/** Only announced Ryhze exclusives belong here. One title does not auto-cycle. */
export const exclusiveTitles: readonly ExclusiveTitle[] = [
  {
    id: 'larcenous-driftscape',
    title: 'Larcenous Driftscape',
    titleLines: ['Larcenous', 'Driftscape'],
    category: 'games',
    description:
      'An open-world crime and driving game set across San Coronado. From the working docks of Oakhaven to the mountain roads of Cascadia, every route is another way into the story.',
    compactDescription: 'Open-world crime and driving in San Coronado.',
    summary: 'An open-world driving project set in San Coronado.',
    mediaLabel: 'Project preview · Not gameplay',
    availability: 'No playable build, download or release date is available yet.',
    genres: ['Open world', 'Driving', 'Crime'],
    setting: 'San Coronado',
    status: 'To Be Continued',
    genre: 'Open world',
    href: '/games/#game-details',
  },
  {
    id: 'tommy-riley',
    title: 'Tommy Riley',
    titleLines: ['Tommy', 'Riley'],
    category: 'films',
    description:
      'Lured by money, an influencer joins an underground organisation. When he regrets his allegiance, he turns against it.',
    compactDescription:
      'An influencer joins an underground organisation for money, then turns against it.',
    summary: 'An influencer joins an underground organisation for money, then turns against it.',
    mediaLabel: 'Project preview · Not film footage',
    availability: 'The film is not available to watch. No release date is available yet.',
    status: 'In development',
    href: '/films/#film-details',
  },
];
