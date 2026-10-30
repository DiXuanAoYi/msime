import {expect, it} from 'vitest';
import {libraryWindow, LIBRARY_ROW_HEIGHT} from './virtual-library-list';

it('covers the viewport and preloads a bounded buffer for large catalogs', () => {
  for (const index of [0, 10, 1000, 13442]) {
    const {start, end} = libraryWindow(13446, index * LIBRARY_ROW_HEIGHT, 520);
    expect(start).toBeLessThanOrEqual(index);
    expect(end).toBeGreaterThanOrEqual(Math.min(13446, index + 4));
    expect(end - start).toBeLessThanOrEqual(48);
  }
});

it('bounds empty, overscrolled, hidden and unusually tall viewports', () => {
  expect(libraryWindow(0, 0, 520)).toEqual({start: 0, end: 0});
  expect(libraryWindow(100, Infinity, 0).start).toBe(0);
  expect(libraryWindow(100, -100, NaN).start).toBe(0);
  expect(libraryWindow(100, 1e12, 520).end).toBe(100);
  const range = libraryWindow(100000, 50000, 100000);
  expect(range.end - range.start).toBeLessThanOrEqual(48);
});

it('preloads enough compact quote rows to cover the viewport without changing the full tail', () => {
  for (const index of [0, 1000, 13438]) {
    const {start, end} = libraryWindow(13446, index * 70, 520, 70);
    expect(start).toBeLessThanOrEqual(index);
    expect(end).toBeGreaterThanOrEqual(Math.min(13446, index + 8));
    expect(end - start).toBeLessThanOrEqual(48);
  }
});
