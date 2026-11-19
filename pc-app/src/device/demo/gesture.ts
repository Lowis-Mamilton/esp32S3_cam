import type { FunctionParams, Result } from '../types'
import { FRAME_H, FRAME_W } from './scene'

/** A simulated hand in 320x240 frame space: (x, y) is the centre of a square box of side `size`. */
export interface DemoHand {
  x: number
  y: number
  size: number
  gesture: number // 0-9, same ids as protocol.md section 10 (0 = hand without a known gesture)
  handScore: number // 0-100, the detector's score
  gestureScore: number // 0-100, the classifier's score
}

const CHANGE_MS = 1500 // the gesture changes every 1.5 s, cycling 0-9
const SECOND_HAND_PERIOD_MS = 12000 // the second hand is there for the second half of every period
const SIZE = 90

const round1 = (n: number) => Math.round(n * 10) / 10

/** Deterministic: the same time always yields the same hands. The two hands roam separate halves so they never overlap. */
export function gestureSceneAt(timeMs: number): DemoHand[] {
  const step = Math.floor(timeMs / CHANGE_MS)
  const wave = (period: number, phase: number) => Math.sin((2 * Math.PI * timeMs) / period + phase)
  const make = (gesture: number, cx: number, phase: number): DemoHand => ({
    x: round1(cx + 40 * wave(9000, phase)),
    y: round1(120 + 35 * wave(7000, phase + 1)),
    size: SIZE,
    gesture,
    handScore: Math.round(80 + 12 * wave(5000, phase + 2)), // 68-92
    gestureScore: gesture === 0 ? 35 : Math.round(78 + 18 * wave(4000, phase + 3)), // 60-96, so the default threshold never hides it
  })
  const hands = [make(step % 10, 100, 0)]
  if (Math.floor(timeMs / (SECOND_HAND_PERIOD_MS / 2)) % 2 === 1) hands.push(make((step + 5) % 10, 220, 2))
  return hands
}

/** Same rules as the firmware (protocol.md section 10): filter by hand score, keep the best maxHands, then name the gesture. */
export function recognizeGestures(hands: DemoHand[], p: FunctionParams['gesture']): Result[] {
  return hands
    .map((h, i) => ({ h, i }))
    .filter(({ h }) => h.handScore >= p.handThreshold)
    .sort((a, b) => b.h.handScore - a.h.handScore || b.h.size - a.h.size || a.i - b.i)
    .slice(0, p.maxHands)
    .map(({ h }): Result => {
      const id = h.gesture >= 1 && h.gestureScore >= p.gestureThreshold ? h.gesture : 0
      return { id, kind: 'gesture', x: h.x, y: h.y, width: h.size, height: h.size, confidence: id ? h.gestureScore : h.handScore }
    })
}

const SKIN = '#f0be96'
const OUTLINE = '#8a5a3c'

/** Draws a cartoon hand for each DemoHand. The only function here that touches a canvas. */
export function drawGestureScene(ctx: CanvasRenderingContext2D, hands: DemoHand[]): void {
  const g = ctx.createLinearGradient(0, 0, FRAME_W, FRAME_H)
  g.addColorStop(0, '#2b3340')
  g.addColorStop(1, '#46505e')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, FRAME_W, FRAME_H)
  for (const h of hands) drawHand(ctx, h)
}

function drawHand(ctx: CanvasRenderingContext2D, h: DemoHand) {
  const u = h.size / 90 // 1 unit = 1 px at the default size
  ctx.save()
  ctx.translate(h.x, h.y + 10 * u)
  ctx.scale(u, u)
  ctx.fillStyle = SKIN
  ctx.strokeStyle = OUTLINE
  ctx.lineWidth = 2
  ctx.lineCap = 'round'
  const finger = (x: number, topY: number, w = 10) => {
    ctx.beginPath()
    ctx.roundRect(x - w / 2, topY, w, 40 - topY, w / 2)
    ctx.fill()
    ctx.stroke()
  }
  const thumb = (angle: number, len = 30) => {
    ctx.save()
    ctx.translate(-22, 12)
    ctx.rotate(angle)
    ctx.beginPath()
    ctx.roundRect(-6, -len, 12, len + 8, 6)
    ctx.fill()
    ctx.stroke()
    ctx.restore()
  }
  const FINGER_X = [-12, -4, 4, 12] // index, middle, ring, little
  const up = (gesture: number): boolean[] => {
    // which of [index, middle, ring, little] are raised
    if (gesture === 1) return [true, false, false, false]
    if (gesture === 2) return [true, true, false, false]
    if (gesture === 3) return [true, true, true, false]
    if (gesture === 4 || gesture === 5) return [true, true, true, true]
    if (gesture === 8) return [false, false, false, true]
    return [false, false, false, false]
  }
  const raised = up(h.gesture)
  // thumb behind the palm for 1-4, out for 5, up for like, down for dislike, out for call, hidden for the rest
  if (h.gesture === 5) thumb(-0.9)
  else if (h.gesture === 6) thumb(-0.1, 36)
  else if (h.gesture === 9) thumb(Math.PI + 0.1, 36)
  else if (h.gesture === 8) thumb(-1.1)
  // fingers
  FINGER_X.forEach((x, i) => {
    const top = raised[i] ? -34 - (i === 1 ? 6 : 0) : 6
    finger(x, top)
  })
  // palm
  ctx.beginPath()
  ctx.roundRect(-22, 4, 44, 36, 12)
  ctx.fill()
  ctx.stroke()
  if (h.gesture >= 1 && h.gesture <= 4) thumb(-0.2, 12) // folded thumb across the palm
  if (h.gesture === 0 || h.gesture === 6 || h.gesture === 9 || h.gesture === 7) {
    // a fist: knuckle line
    ctx.beginPath()
    ctx.moveTo(-14, 10)
    ctx.lineTo(14, 10)
    ctx.stroke()
  }
  if (h.gesture === 7) {
    // OK: thumb and index make a ring
    ctx.beginPath()
    ctx.arc(-16, -4, 10, 0, Math.PI * 2)
    ctx.stroke()
    finger(4, -34)
    finger(12, -34)
    finger(20, -30)
  }
  ctx.restore()
}
