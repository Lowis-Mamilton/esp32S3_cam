import type { FunctionId, FunctionParams, LearnedId, Result } from '../types'
import { FRAME_H, FRAME_W, type SceneObject } from './scene'

type RGB = [number, number, number]
type Region = { x: number; y: number; width: number; height: number }

/** Rejection reason from learnColor; `code` is an i18n key suffix. */
export class DemoLearnError extends Error {
  readonly code: 'no-color' | 'full'
  constructor(code: 'no-color' | 'full') {
    super(code)
    this.name = 'DemoLearnError'
    this.code = code
  }
}

export function rgbToHsv([r, g, b]: RGB): [number, number, number] {
  const R = r / 255
  const G = g / 255
  const B = b / 255
  const max = Math.max(R, G, B)
  const d = max - Math.min(R, G, B)
  let h = 0
  if (d > 0) {
    if (max === R) h = ((G - B) / d) % 6
    else if (max === G) h = (B - R) / d + 2
    else h = (R - G) / d + 4
    h *= 60
    if (h < 0) h += 360
  }
  return [h, max === 0 ? 0 : d / max, max]
}

const hueDiff = (a: number, b: number) => Math.min(Math.abs(a - b), 360 - Math.abs(a - b))

/**
 * tolerance (0-100) -> match window:
 *   hue window   = +-(tolerance * 0.6) degrees  (30 -> +-18, 100 -> +-60)
 *   saturation / value must each differ by <= 0.25 + tolerance/200  (30 -> 0.4)
 * Colours with saturation < 0.15 have no meaningful hue and never match.
 * Note: a high tolerance widens the window enough that two learned colours (e.g. red and orange)
 * both match one blob; the nearest hue wins, so close colours can be merged into one ID.
 */
export function colorMatches(a: RGB, b: RGB, tolerance: number): boolean {
  const [ha, sa, va] = rgbToHsv(a)
  const [hb, sb, vb] = rgbToHsv(b)
  if (sa < 0.15 || sb < 0.15) return false
  const sv = 0.25 + tolerance / 200
  return hueDiff(ha, hb) <= tolerance * 0.6 && Math.abs(sa - sb) <= sv && Math.abs(va - vb) <= sv
}

/** The learned entry closest in hue to `rgb` within tolerance, or null. */
export function matchLearned(rgb: RGB, learned: LearnedId[], tolerance: number): LearnedId | null {
  let best: LearnedId | null = null
  let bestDh = Infinity
  const h = rgbToHsv(rgb)[0]
  for (const l of learned) {
    if (!l.color || !colorMatches(rgb, l.color, tolerance)) continue
    const d = hueDiff(h, rgbToHsv(l.color)[0])
    if (d < bestDh) {
      best = l
      bestDh = d
    }
  }
  return best
}

interface Clipped {
  x: number
  y: number
  width: number
  height: number
  visible: number // visible fraction of the bounding box (approximation for area clipping)
}

/** Bounding box clipped to the frame (integers); null when fully outside. */
function clip(o: SceneObject): Clipped | null {
  const x0 = Math.max(0, o.x - o.r)
  const x1 = Math.min(FRAME_W, o.x + o.r)
  const y0 = Math.max(0, o.y - o.r)
  const y1 = Math.min(FRAME_H, o.y + o.r)
  if (x1 - x0 < 1 || y1 - y0 < 1) return null
  return {
    x: Math.round((x0 + x1) / 2),
    y: Math.round((y0 + y1) / 2),
    width: Math.round(x1 - x0),
    height: Math.round(y1 - y0),
    visible: ((x1 - x0) / (2 * o.r)) * ((y1 - y0) / (2 * o.r)),
  }
}

/** Deterministic 70-98 from the object's position, which is itself a function of time. */
const faceConfidence = (o: SceneObject) => Math.round(84 + 14 * Math.sin(o.x * 0.05 + o.y * 0.03))

export function recognize(fn: FunctionId, objs: SceneObject[], learned: LearnedId[], params: FunctionParams): Result[] {
  const out: { area: number; result: Result }[] = []
  for (const o of objs) {
    const b = clip(o)
    if (!b) continue
    const box = { x: b.x, y: b.y, width: b.width, height: b.height }
    if (fn === 1 && o.kind === 'blob' && o.rgb) {
      const area = Math.PI * o.r * o.r * b.visible
      if (area < params.color.minArea) continue
      const m = matchLearned(o.rgb, learned, params.color.tolerance)
      out.push({ area, result: { ...box, id: m?.id ?? 0, kind: 'color' } })
    } else if (fn === 2 && o.kind === 'tag') {
      out.push({ area: 4 * o.r * o.r * b.visible, result: { ...box, id: o.tagId ?? 0, kind: 'tag' } })
    } else if (fn === 3 && o.kind === 'face') {
      const confidence = faceConfidence(o)
      if (confidence < params.face.threshold) continue
      out.push({ area: Math.PI * o.r * o.r * b.visible, result: { ...box, id: 0, kind: 'face', confidence } })
    }
  }
  out.sort((a, b) => b.area - a.area)
  const cap = fn === 1 ? params.color.maxResults : fn === 3 ? params.face.maxResults : 8
  return out.slice(0, cap).map((e) => e.result)
}

/** Colour of the blob covering most of the region (topmost wins overlaps); null on background. */
export function colorAt(objs: SceneObject[], region: Region): RGB | null {
  const step = Math.max(1, Math.ceil(Math.max(region.width, region.height) / 48))
  const counts = new Map<SceneObject, number>()
  for (let y = region.y; y < region.y + region.height; y += step) {
    for (let x = region.x; x < region.x + region.width; x += step) {
      if (x < 0 || y < 0 || x >= FRAME_W || y >= FRAME_H) continue
      for (let i = objs.length - 1; i >= 0; i--) {
        const o = objs[i]
        if (o.kind === 'blob' && o.rgb && (x - o.x) ** 2 + (y - o.y) ** 2 <= o.r * o.r) {
          counts.set(o, (counts.get(o) ?? 0) + 1)
          break
        }
      }
    }
  }
  let best: SceneObject | null = null
  let n = 0
  for (const [o, c] of counts) {
    if (c > n) {
      best = o
      n = c
    }
  }
  return best?.rgb ? [...best.rgb] : null
}
