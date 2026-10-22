import assert from 'node:assert/strict';
import test from 'node:test';
import { scriptModule } from './helpers/script-module.mjs';

const { frameMotion } = scriptModule('frame-motion');
const style = (tokens) => ({ getPropertyValue: (name) => tokens[name] || '' });

test('frame motion defaults are shared and safe when styles are unavailable', () => {
  for (const [opening, duration] of [
    [true, 360],
    [false, 280],
  ]) {
    const result = frameMotion(null, opening);
    assert.equal(result.duration, duration);
    assert.equal(result.easing, 'cubic-bezier(0.22, 0.68, 0.18, 1)');
    assert.equal(result.fill, 'both');
  }
});

test('frame duration accepts milliseconds, seconds, fractions and explicit zero', () => {
  for (const [token, duration] of [
    ['625ms', 625],
    [' .45s ', 450],
    ['1.25s', 1250],
    ['0ms', 0],
  ]) {
    for (const opening of [true, false]) {
      const result = frameMotion(
        style({
          [`--duration-frame-${opening ? 'enter' : 'exit'}`]: token,
          '--ease-frame': ' ease-in-out ',
        }),
        opening,
      );
      assert.equal(result.duration, duration, token);
      assert.equal(result.easing, 'ease-in-out');
    }
  }
});

test('malformed, negative, nonfinite and unitless durations fall back safely', () => {
  for (const token of [
    '',
    '-1ms',
    '700',
    '20junk',
    '1s trailing',
    'Infinityms',
    `${'9'.repeat(400)}s`,
  ]) {
    const tokens = { '--duration-frame-enter': token, '--duration-frame-exit': token };
    assert.equal(frameMotion(style(tokens), true).duration, 360, token);
    assert.equal(frameMotion(style(tokens), false).duration, 280, token);
  }
});
