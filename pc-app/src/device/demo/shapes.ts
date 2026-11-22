import type { FunctionParams, Result } from '../types'
import { FRAME_H, FRAME_W } from './scene'

/**
 * A simulated shape drawn on white paper, in 320x240 frame space. id is the protocol shape id (1 circle, 2 triangle,
 * 3 square, 4 rectangle); `size` is the diameter, the side, or the rectangle's long side; angleDeg turns it clockwise.
 */
export interface DemoShape {
  id: number
  cx: number
  cy: number
  size: number
  angleDeg: number
}

/** A rectangle's short side relative to its long side. */
const RECT_ASPECT = 0.6
/** Paper margin around the frame: the paper is the background, the shapes lie inside it. */
const PAPER = 8

const round1 = (n: number) => Math.round(n * 10) / 10
const wave = (t: number, period: number, phase = 0) => Math.sin((2 * Math.PI * t) / period + phase)

/**
 * Deterministic: the same time always yields the same shapes. Each shape drifts in its own quarter of the frame
 * (circle top-left, triangle top-right, square bottom-left, rectangle bottom-right), so they never touch each other
 * or the frame edge; the triangle and the rectangle rock, the square turns.
 */
export function shapeSceneAt(timeMs: number): DemoShape[] {
  const drift = (cx: number, cy: number, phase: number) => ({
    cx: round1(cx + 18 * wave(timeMs, 16000, phase)),
    cy: round1(cy + 12 * wave(timeMs, 12000, phase + 1)),
  })
  return [
    { id: 1, ...drift(80, 70, 0), size: 60, angleDeg: 0 },
    { id: 2, ...drift(240, 72, 1.3), size: 66, angleDeg: round1(20 * wave(timeMs, 10000)) },
    { id: 3, ...drift(80, 170, 2.6), size: 54, angleDeg: round1(((timeMs * 90) / 20000) % 90) },
    { id: 4, ...drift(240, 170, 3.9), size: 72, angleDeg: round1(15 * wave(timeMs, 14000, 2)) },
  ]
}

/** Outline points of a shape on screen (a circle as 32 points). */
export function shapeOutline(s: DemoShape): [number, number][] {
  const a = (s.angleDeg * Math.PI) / 180
  const turn = ([x, y]: [number, number]): [number, number] => [s.cx + x * Math.cos(a) - y * Math.sin(a), s.cy + x * Math.sin(a) + y * Math.cos(a)]
  const h = s.size / 2
  if (s.id === 1) return Array.from({ length: 32 }, (_, i) => turn([h * Math.cos((i * Math.PI) / 16), h * Math.sin((i * Math.PI) / 16)]))
  if (s.id === 2) {
    const r = s.size / Math.sqrt(3) // circumradius of an equilateral triangle, centred on its centroid
    return [-90, 30, 150].map((d) => turn([r * Math.cos((d * Math.PI) / 180), r * Math.sin((d * Math.PI) / 180)]))
  }
  const k = s.id === 4 ? h * RECT_ASPECT : h
  return [[-h, -k], [h, -k], [h, k], [-h, k]].map((p) => turn(p as [number, number]))
}

/** Polygon area (shoelace); the circle's 32 points are close enough to πr². */
const area = (pts: [number, number][]) =>
  Math.abs(pts.reduce((s, [x, y], i) => {
    const [nx, ny] = pts[(i + 1) % pts.length]
    return s + x * ny - nx * y
  }, 0)) / 2

/** Steady, plausible fit scores (outline area against the ideal shape), varying a little with position. */
const BASE_CONFIDENCE: Record<number, number> = { 1: 92, 2: 88, 3: 90, 4: 86 }

/**
 * What the firmware does (docs/protocol.md section 11), on the synthetic scene: the shapes are dark on light paper, so
 * the light mode finds nothing (the demo has no grey levels, `threshold` has no effect); shapes touching the frame edge
 * and those whose box's long side is below `minSize` are dropped; the largest `maxResults` by area are reported.
 */
export function detectShapes(shapes: DemoShape[], p: FunctionParams['shape']): Result[] {
  if (p.mode === 'light') return []
  const out: { area: number; r: Result }[] = []
  for (const s of shapes) {
    const pts = shapeOutline(s)
    const xs = pts.map((q) => q[0])
    const ys = pts.map((q) => q[1])
    const [x0, x1, y0, y1] = [Math.floor(Math.min(...xs)), Math.ceil(Math.max(...xs)), Math.floor(Math.min(...ys)), Math.ceil(Math.max(...ys))]
    if (x0 <= 0 || y0 <= 0 || x1 >= FRAME_W - 1 || y1 >= FRAME_H - 1) continue
    const width = x1 - x0 + 1
    const height = y1 - y0 + 1
    if (Math.max(width, height) < p.minSize) continue
    const confidence = Math.round((BASE_CONFIDENCE[s.id] ?? 80) + 3 * Math.sin(s.cx * 0.07 + s.cy * 0.05))
    out.push({ area: area(pts), r: { id: s.id, kind: 'shape', x: Math.floor((x0 + x1) / 2), y: Math.floor((y0 + y1) / 2), width, height, confidence } })
  }
  out.sort((a, b) => b.area - a.area)
  return out.slice(0, p.maxResults).map((e) => e.r)
}

const INK: Record<number, string> = { 1: '#1d3557', 2: '#2b2d42', 3: '#3a2e39', 4: '#1b4332' }

/** A table, a sheet of white paper and the dark shapes on it. The only function here that touches a canvas. */
export function drawShapeScene(ctx: CanvasRenderingContext2D, shapes: DemoShape[]): void {
  ctx.fillStyle = '#5b6470'
  ctx.fillRect(0, 0, FRAME_W, FRAME_H)
  ctx.fillStyle = '#f4f2ec'
  ctx.fillRect(PAPER, PAPER, FRAME_W - 2 * PAPER, FRAME_H - 2 * PAPER)
  for (const s of shapes) {
    ctx.fillStyle = INK[s.id] ?? '#222'
    ctx.beginPath()
    if (s.id === 1) ctx.arc(s.cx, s.cy, s.size / 2, 0, Math.PI * 2)
    else
      shapeOutline(s).forEach(([x, y], i) => {
        if (i === 0) ctx.moveTo(x, y)
        else ctx.lineTo(x, y)
      })
    ctx.closePath()
    ctx.fill()
  }
}
