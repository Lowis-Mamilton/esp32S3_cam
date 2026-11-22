import type { LearnedId, Result } from '../types'
import { FRAME_H, FRAME_W } from './scene'

/**
 * A simulated QR code card in 320x240 frame space: (cx, cy) is the centre, `size` the side of the code itself
 * (without its white margin), `angleDeg` the rotation on screen (clockwise positive, like the protocol).
 */
export interface DemoQr {
  cx: number
  cy: number
  size: number
  angleDeg: number
  text: string
}

/** The demo's QR results report at most this many codes, like the firmware (MXQ_MAX_CODES). */
const MAX_CODES = 4
/** Modules per side (version 1) and the white margin around them, in modules. */
const MODULES = 21
const QUIET = 2

const round1 = (n: number) => Math.round(n * 10) / 10
const wave = (t: number, period: number, phase = 0) => Math.sin((2 * Math.PI * t) / period + phase)

/**
 * Deterministic: the same time always yields the same cards. Two cards drift slowly in separate halves of the frame
 * (so they never overlap and always stay whole inside it): 'MXCam' rocks back and forth, 'LEFT' turns all the way round.
 */
export function qrSceneAt(timeMs: number): DemoQr[] {
  const spin = ((timeMs * 360) / 30000) % 360
  return [
    { cx: round1(82 + 24 * wave(timeMs, 17000)), cy: round1(120 + 45 * wave(timeMs, 13000, 1)), size: 64, angleDeg: round1(20 * wave(timeMs, 9000)), text: 'MXCam' },
    { cx: round1(240 + 30 * wave(timeMs, 15000, 2)), cy: round1(115 + 55 * wave(timeMs, 11000, 0.5)), size: 52, angleDeg: round1(spin > 180 ? spin - 360 : spin), text: 'LEFT' },
  ]
}

/** The code's own corners (top-left, top-right, bottom-right, bottom-left), rotated on screen, in whole pixels. */
export function qrCorners(c: DemoQr): [number, number][] {
  const a = (c.angleDeg * Math.PI) / 180
  const h = c.size / 2
  const local: [number, number][] = [[-h, -h], [h, -h], [h, h], [-h, h]]
  return local.map(([x, y]) => [Math.round(c.cx + x * Math.cos(a) - y * Math.sin(a)), Math.round(c.cy + x * Math.sin(a) + y * Math.cos(a))])
}

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v))

/**
 * What the firmware reports (docs/protocol.md section 11): one record per code that is whole in the frame (a cut code
 * cannot be decoded), box = corners' bounding box clipped to the frame (centre (min + max) / 2, size max - min + 1),
 * the rotation and the unclipped corners, id = the learned ID with the same content (0 = not learned), no confidence;
 * the largest 4 by area.
 */
export function detectQr(cards: DemoQr[], learned: LearnedId[]): Result[] {
  const out: { area: number; r: Result }[] = []
  for (const c of cards) {
    const corners = qrCorners(c)
    if (corners.some(([x, y]) => x < 0 || y < 0 || x >= FRAME_W || y >= FRAME_H)) continue
    const xs = corners.map((p) => clamp(p[0], 0, FRAME_W - 1))
    const ys = corners.map((p) => clamp(p[1], 0, FRAME_H - 1))
    const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)]
    const [tl, tr] = corners
    out.push({
      area: c.size * c.size,
      r: {
        id: learned.find((l) => l.text === c.text)?.id ?? 0,
        kind: 'qr',
        x: Math.floor((x0 + x1) / 2),
        y: Math.floor((y0 + y1) / 2),
        width: x1 - x0 + 1,
        height: y1 - y0 + 1,
        rotationDeg: Math.round((Math.atan2(tr[1] - tl[1], tr[0] - tl[0]) * 180 * 100) / Math.PI) / 100,
        corners,
        text: c.text,
        truncated: false,
      },
    })
  }
  out.sort((a, b) => b.area - a.area)
  return out.slice(0, MAX_CODES).map((e) => e.r)
}

/** Data modules: a fixed pseudo-random pattern per content (looks like a QR code; not a decodable one). */
function moduleOn(text: string, col: number, row: number): boolean {
  let h = 2166136261
  for (const ch of `${text}:${col}:${row}`) h = Math.imul(h ^ ch.charCodeAt(0), 16777619)
  return ((h >>> 0) & 0x100) !== 0
}
const inFinder = (col: number, row: number) => (col < 8 && row < 8) || (col >= MODULES - 8 && row < 8) || (col < 8 && row >= MODULES - 8)

/** A desk with the cards. The only function here that touches a canvas. */
export function drawQrScene(ctx: CanvasRenderingContext2D, cards: DemoQr[]): void {
  const g = ctx.createLinearGradient(0, 0, FRAME_W, FRAME_H)
  g.addColorStop(0, '#8a6e52')
  g.addColorStop(1, '#6f5640')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, FRAME_W, FRAME_H)
  for (const c of cards) {
    const m = c.size / MODULES
    ctx.save()
    ctx.translate(c.cx, c.cy)
    ctx.rotate((c.angleDeg * Math.PI) / 180)
    ctx.translate(-c.size / 2, -c.size / 2) // origin at the code's top-left corner
    ctx.fillStyle = '#fff'
    ctx.fillRect(-QUIET * m, -QUIET * m, (MODULES + 2 * QUIET) * m, (MODULES + 2 * QUIET) * m)
    ctx.fillStyle = '#111'
    for (const [fx, fy] of [[0, 0], [MODULES - 7, 0], [0, MODULES - 7]]) {
      ctx.fillRect(fx * m, fy * m, 7 * m, 7 * m)
      ctx.fillStyle = '#fff'
      ctx.fillRect((fx + 1) * m, (fy + 1) * m, 5 * m, 5 * m)
      ctx.fillStyle = '#111'
      ctx.fillRect((fx + 2) * m, (fy + 2) * m, 3 * m, 3 * m)
    }
    for (let row = 0; row < MODULES; row++)
      for (let col = 0; col < MODULES; col++) if (!inFinder(col, row) && moduleOn(c.text, col, row)) ctx.fillRect(col * m, row * m, m, m)
    ctx.restore()
  }
}
