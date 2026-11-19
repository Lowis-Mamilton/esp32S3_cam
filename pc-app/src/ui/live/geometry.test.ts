import { expect, test } from 'vitest'
import { toFrame, toScreen } from './geometry'

test('wide viewport letterboxes left/right', () => {
  // 800x300: scale = min(2.5, 1.25) = 1.25 -> frame is 400x300, offset x = 200
  const vp = { width: 800, height: 300 }
  expect(toScreen(vp, 0, 0)).toEqual({ px: 200, py: 0 })
  expect(toScreen(vp, 320, 240)).toEqual({ px: 600, py: 300 })
  expect(toFrame(vp, 400, 150)).toEqual({ x: 160, y: 120 })
  expect(toFrame(vp, 100, 150).x).toBeLessThan(0) // in the black bar, not clamped here
})

test('tall viewport letterboxes top/bottom', () => {
  // 320x480: scale = 1 -> frame is 320x240, offset y = 120
  const vp = { width: 320, height: 480 }
  expect(toScreen(vp, 0, 0)).toEqual({ px: 0, py: 120 })
  expect(toScreen(vp, 160, 120)).toEqual({ px: 160, py: 240 })
  expect(toFrame(vp, 160, 240)).toEqual({ x: 160, y: 120 })
  expect(toFrame(vp, 0, 0).y).toBe(-120)
})

test('toFrame(toScreen(p)) round-trips in both orientations', () => {
  for (const vp of [
    { width: 800, height: 300 },
    { width: 320, height: 480 },
    { width: 1000, height: 750 },
  ]) {
    for (const [x, y] of [[0, 0], [17, 203], [160, 120], [320, 240]]) {
      const s = toScreen(vp, x, y)
      const f = toFrame(vp, s.px, s.py)
      expect(f.x).toBeCloseTo(x, 6)
      expect(f.y).toBeCloseTo(y, 6)
    }
  }
})
