/** Keep CSS controls and Web Animations on one frame-motion contract. */
export function frameMotion(
  style: CSSStyleDeclaration | null,
  opening: boolean,
): KeyframeAnimationOptions & { duration: number } {
  const fallback = opening ? 360 : 280;
  const token =
    style?.getPropertyValue(`--duration-frame-${opening ? 'enter' : 'exit'}`).trim() || '';
  const time = /^(\d+(?:\.\d+)?|\.\d+)(ms|s)$/.exec(token);
  const duration = time ? Number(time[1]) * (time[2] === 's' ? 1000 : 1) : fallback;
  return {
    duration: Number.isFinite(duration) ? duration : fallback,
    easing: style?.getPropertyValue('--ease-frame').trim() || 'cubic-bezier(0.22, 0.68, 0.18, 1)',
    fill: 'both',
  };
}
