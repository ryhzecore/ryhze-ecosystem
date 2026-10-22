export const coreServices = [
  'Foundation',
  'Platform Abstraction',
  'Memory',
  'Jobs',
  'Timing',
  'Configuration',
  'Diagnostics',
  'Runtime Lifecycle',
];
export const runtimeAreas = [
  'Scene & World',
  'Assets & Resources',
  'Graphics & Rendering',
  'Animation & Cinematics',
  'Physics & Simulation',
  'Audio',
  'Gameplay Framework',
  'Scripting & Visual Scripting',
  'Networking & Multiplayer',
  'UI & Input',
];
export const authoringAreas = ['Editor & Authoring', 'Film/Mocap/VFX tools'];
export const outputAreas = ['Games', 'Films/Cinematics'];
export const sceneMilestone = {
  status: 'First Linux headless milestone signed off',
  summary:
    'The first Linux headless milestone passed its test gates and is signed off after source and evidence review.',
};
export const assetsMilestone = {
  status: 'First Linux headless milestone signed off',
  summary:
    'The first Linux headless milestone is signed off after source and evidence review and an independent full review.',
  capability:
    'Assets & Resources prepares validated resource data and loads it asynchronously with bounded memory, dependency tracking and cancellation.',
  limits:
    'Native scene and mesh/material packages now load into the software renderer. Validation covers CPU/software fixtures only. Other asset types, media importers, GPU resource management, reload and network streaming remain Under Development.',
};

export const vfxMilestone = {
  progress:
    'Bounded CPU mesh particles now run in the software renderer, with fixed-step motion, repeatable emission and tested lifetimes. Broader VFX and authoring tools remain Under Development.',
};

export const physicsMilestone = {
  progress:
    'Bounded CPU physics now supports spheres and a static plane, with fixed-step motion, collision response and saved-state recovery. Broader physics and simulation remain Under Development.',
};

export const audioMilestone = {
  progress:
    'Bounded offline audio now supports native sound assets and collision cues. Device playback and spatial audio remain Under Development.',
};

export const atmosphereMilestone = {
  progress:
    'A bounded native CPU atmosphere reference now models sky scattering, sunlight attenuation and distance haze. Vulkan atmosphere rendering, weather and celestial content remain Under Development.',
};
