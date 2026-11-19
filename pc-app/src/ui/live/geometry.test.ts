import { expect, test } from 'vitest'
import { boxToScreen, mirrorRegion, toFrame, toScreen } from './geometry'

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

// ---- mirror view (ADR-0010): protocol coordinates are the TRUE image; the screen may show its mirror ----
test('mirror: toScreen flips x around the frame centre, toFrame inverts it', () => {
  const vp = { width: 640, height: 480 } // scale 2, no offset
  expect(toScreen(vp, 0, 10, true)).toEqual({ px: 640, py: 20 })
  expect(toScreen(vp, 320, 10, true)).toEqual({ px: 0, py: 20 })
  expect(toScreen(vp, 100, 10, false)).toEqual({ px: 200, py: 20 })
  expect(toFrame(vp, 600, 20, true)).toEqual({ x: 20, y: 10 })
  expect(toFrame(vp, 600, 20, false)).toEqual({ x: 300, y: 10 })
  const wide = { width: 800, height: 300 }
  for (const [x, y] of [[0, 0], [17, 203], [320, 240]]) {
    const s = toScreen(wide, x, y, true)
    const f = toFrame(wide, s.px, s.py, true)
    expect(f.x).toBeCloseTo(x, 6)
    expect(f.y).toBeCloseTo(y, 6)
  }
})

test('boxToScreen places a centre-based box in both modes', () => {
  const vp = { width: 640, height: 480 }
  const r = { x: 50, y: 100, width: 20, height: 40 } // true-image rect 40..60 x 80..120
  expect(boxToScreen(vp, r, false)).toEqual({ left: 80, top: 160, width: 40, height: 80 })
  expect(boxToScreen(vp, r, true)).toEqual({ left: 520, top: 160, width: 40, height: 80 }) // 260..280 mirrored
})

test('mirrorRegion maps a top-left region between the true image and its mirror', () => {
  expect(mirrorRegion({ x: 10, y: 5, width: 30, height: 8 })).toEqual({ x: 280, y: 5, width: 30, height: 8 })
  expect(mirrorRegion({ x: 152, y: 112, width: 16, height: 16 })).toEqual({ x: 152, y: 112, width: 16, height: 16 })
  const r = { x: 3, y: 4, width: 50, height: 6 }
  expect(mirrorRegion(mirrorRegion(r))).toEqual(r)
})
