import type { FunctionParams, Result } from '../types'
import { clipObject } from './recognize'
import type { SceneObject } from './scene'

type Region = { x: number; y: number; width: number; height: number }

const MIN_OVERLAP = 0.3

/**
 * The object a selection means: the one whose bounding box overlaps the region at least 30% (intersection over the
 * smaller of the two areas, so a selection inside a big object counts), the best overlap winning; null when nothing does.
 */
export function pickTarget(objs: SceneObject[], region: Region): number | null {
  let best: number | null = null
  let bestRatio = 0
  objs.forEach((o, i) => {
    const w = Math.max(0, Math.min(region.x + region.width, o.x + o.r) - Math.max(region.x, o.x - o.r))
    const h = Math.max(0, Math.min(region.y + region.height, o.y + o.r) - Math.max(region.y, o.y - o.r))
    const ratio = (w * h) / Math.min(region.width * region.height, 4 * o.r * o.r)
    if (ratio >= MIN_OVERLAP && ratio > bestRatio) {
      best = i
      bestRatio = ratio
    }
  })
  return best
}

/** Average colour of a scene object, as the board would report for a learned object. */
export function objectColor(o: SceneObject): [number, number, number] {
  if (o.rgb) return [...o.rgb]
  return o.kind === 'face' ? [240, 190, 150] : [128, 128, 128]
}

/** The tracked object box, or nothing when it is out of the frame or its match score is below `threshold`. */
export function trackResult(o: SceneObject, p: FunctionParams['track']): Result[] {
  const b = clipObject(o)
  if (!b) return []
  const confidence = Math.round(55 + 40 * b.visible) // a half-hidden object matches worse
  if (confidence < p.threshold) return []
  return [{ id: 1, kind: 'track', x: b.x, y: b.y, width: b.width, height: b.height, confidence }]
}
