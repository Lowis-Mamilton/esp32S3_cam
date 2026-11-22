import type { FunctionParams, Result } from '../types'
import { FRAME_H, FRAME_W } from './scene'

/** A ball rolling across a still room, in 320x240 frame space: centre (x, y), radius r; `moving` while it rolls. */
export interface DemoMotion {
  x: number
  y: number
  r: number
  moving: boolean
}

/** Each leg: the ball rolls for ROLL_MS, then rests for the rest of LEG_MS (and blends into the background). */
const LEG_MS = 5000
const ROLL_MS = 3000
const R = 18
const X_FROM = 50
const X_TO = 270
/** Grey-level difference between the ball and the floor: thresholds above it see no motion. */
const CONTRAST = 60

const round1 = (n: number) => Math.round(n * 10) / 10

/** Deterministic: the same time always yields the same scene. The ball rolls left to right, rests, rolls back, rests. */
export function motionSceneAt(timeMs: number): DemoMotion {
  const leg = Math.floor(timeMs / LEG_MS)
  const within = timeMs - leg * LEG_MS
  const p = Math.min(1, within / ROLL_MS)
  const eased = (1 - Math.cos(Math.PI * p)) / 2
  const f = leg % 2 === 0 ? eased : 1 - eased
  return {
    x: round1(X_FROM + (X_TO - X_FROM) * f),
    y: round1(165 - 45 * Math.sin(Math.PI * f)), // a small hop over the middle
    r: R,
    moving: within < ROLL_MS,
  }
}

/**
 * What the firmware reports (docs/protocol.md section 11) for this scene: one moving region (id 0) while the ball
 * rolls and nothing while it rests; nothing when `threshold` is above the ball's contrast or the box's long side is
 * below `minSize`. confidence = how much of the box really changed (a disc fills about 78% of its box).
 */
export function detectMotion(s: DemoMotion, p: FunctionParams['motion']): Result[] {
  if (!s.moving || p.threshold > CONTRAST) return []
  const x0 = Math.max(0, Math.floor(s.x - s.r))
  const x1 = Math.min(FRAME_W - 1, Math.ceil(s.x + s.r))
  const y0 = Math.max(0, Math.floor(s.y - s.r))
  const y1 = Math.min(FRAME_H - 1, Math.ceil(s.y + s.r))
  const width = x1 - x0 + 1
  const height = y1 - y0 + 1
  if (x1 < x0 || y1 < y0 || Math.max(width, height) < p.minSize) return []
  return [{ id: 0, kind: 'motion', x: Math.floor((x0 + x1) / 2), y: Math.floor((y0 + y1) / 2), width, height, confidence: 78 }]
}

/** A still room (wall, floor, a shelf) and the ball. The only function here that touches a canvas. */
export function drawMotionScene(ctx: CanvasRenderingContext2D, s: DemoMotion): void {
  ctx.fillStyle = '#c9c3b6' // wall
  ctx.fillRect(0, 0, FRAME_W, 150)
  ctx.fillStyle = '#8c7a64' // floor
  ctx.fillRect(0, 150, FRAME_W, FRAME_H - 150)
  ctx.fillStyle = '#6d5a45' // shelf
  ctx.fillRect(20, 40, 90, 10)
  ctx.fillRect(210, 70, 80, 10)
  ctx.fillStyle = '#e8743b' // ball
  ctx.beginPath()
  ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2)
  ctx.fill()
}
