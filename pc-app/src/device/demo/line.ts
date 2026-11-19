import type { FunctionParams, Result } from '../types'
import { FRAME_H, FRAME_W } from './scene'

/** A dark line on a light floor, in 320x240 frame space. `path(y)` is the line centre x at row y. */
export interface LineScene {
  path: (y: number) => number
  /** Row of a horizontal bar across the whole frame (a crossing), or null. */
  crossY: number | null
  /** Line width in px (also the bar thickness). */
  width: number
}

const WIDTH = 14
const CROSS_PERIOD_MS = 12000 // the bar is there for the last third of every period

/** Deterministic: the same time always yields the same scene. The line gently snakes and drifts, staying inside the frame. */
export function lineSceneAt(timeMs: number): LineScene {
  const drift = 55 * Math.sin((2 * Math.PI * timeMs) / 14000)
  const wobble = (y: number) => 28 * Math.sin(y / 38 + (2 * Math.PI * timeMs) / 9000)
  const inPeriod = timeMs % CROSS_PERIOD_MS
  return {
    path: (y) => 160 + drift + wobble(y),
    crossY: inPeriod >= (CROSS_PERIOD_MS * 2) / 3 ? 130 : null,
    width: WIDTH,
  }
}

const RAD_TO_DEG = 180 / Math.PI

/**
 * What the firmware does (docs/protocol.md section 9), on the synthetic scene: look at the lower `roiPercent` of the
 * frame, cut it into `bands` horizontal bands, take the dark run in each band (a run wider than `maxWidth` is a crossing
 * and is not part of the line; narrower than `minWidth` is noise), fit x = a + b*y by least squares, and report the one
 * main line, or nothing when fewer than 2 bands found it. The demo has no grey levels, so `threshold` has no effect;
 * a light line never exists in the scene, and the learned-colour mode follows the dark line.
 */
export function detectLine(scene: LineScene, p: FunctionParams['line']): Result[] {
  if (p.mode === 'light') return []
  const top = FRAME_H - Math.round((FRAME_H * p.roiPercent) / 100)
  const bandH = (FRAME_H - top) / p.bands
  const pts: [number, number][] = []
  let cross = false
  for (let i = 0; i < p.bands; i++) {
    const y = top + (i + 0.5) * bandH
    if (scene.crossY !== null && Math.abs(scene.crossY - y) <= (bandH + scene.width) / 2) {
      cross = cross || FRAME_W > p.maxWidth
      continue
    }
    const x0 = Math.max(0, scene.path(y) - scene.width / 2)
    const x1 = Math.min(FRAME_W, scene.path(y) + scene.width / 2)
    const w = x1 - x0
    if (w < p.minWidth) continue
    if (w > p.maxWidth) {
      cross = true
      continue
    }
    pts.push([(x0 + x1) / 2, y])
  }
  if (pts.length < 2) return []
  // x = a + b*y by least squares
  const n = pts.length
  const my = pts.reduce((s, q) => s + q[1], 0) / n
  const mx = pts.reduce((s, q) => s + q[0], 0) / n
  const syy = pts.reduce((s, q) => s + (q[1] - my) ** 2, 0)
  const b = pts.reduce((s, q) => s + (q[0] - mx) * (q[1] - my), 0) / syy
  const a = mx - b * my
  const yBottom = pts[n - 1][1]
  const yTop = pts[0][1]
  const start: [number, number] = [Math.round(a + b * yBottom), Math.round(yBottom)]
  const end: [number, number] = [Math.round(a + b * yTop), Math.round(yTop)]
  return [
    {
      id: 1,
      kind: 'line',
      x: Math.round((start[0] + end[0]) / 2),
      y: Math.round((start[1] + end[1]) / 2),
      width: Math.max(2, Math.abs(end[0] - start[0]) + 1),
      height: Math.max(2, Math.abs(start[1] - end[1]) + 1),
      confidence: Math.floor((n * 100) / p.bands),
      line: {
        angleDeg: Math.round(Math.atan(-b) * RAD_TO_DEG * 100) / 100,
        offsetX: start[0] - 160,
        start,
        end,
        branch: false,
        cross,
      },
    },
  ]
}

/** Light floor, black tape, and a bar when the scene has one. The only function here that touches a canvas. */
export function drawLineScene(ctx: CanvasRenderingContext2D, timeMs: number): void {
  const scene = lineSceneAt(timeMs)
  const g = ctx.createLinearGradient(0, 0, FRAME_W, FRAME_H)
  g.addColorStop(0, '#e6e1d6')
  g.addColorStop(1, '#cfcabd')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, FRAME_W, FRAME_H)
  ctx.fillStyle = '#111'
  if (scene.crossY !== null) ctx.fillRect(0, scene.crossY - scene.width / 2, FRAME_W, scene.width)
  ctx.strokeStyle = '#111'
  ctx.lineWidth = scene.width
  ctx.lineJoin = 'round'
  ctx.beginPath()
  for (let y = 0; y <= FRAME_H; y += 4) {
    if (y === 0) ctx.moveTo(scene.path(y), y)
    else ctx.lineTo(scene.path(y), y)
  }
  ctx.stroke()
}
