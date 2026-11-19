export const FRAME_W = 320
export const FRAME_H = 240

/** Displayed size of the stage box in CSS pixels. */
export interface Viewport {
  width: number
  height: number
}

/** Uniform scale that fits the 320x240 frame inside the viewport, plus the centring offsets. */
function fit(vp: Viewport) {
  const scale = Math.min(vp.width / FRAME_W, vp.height / FRAME_H)
  return { scale, ox: (vp.width - FRAME_W * scale) / 2, oy: (vp.height - FRAME_H * scale) / 2 }
}

/**
 * Screen (relative to the stage box) to frame coordinates. Not clamped: callers decide.
 * Frame coordinates are always the TRUE camera image (protocol, ADR-0010); `mirror` means the screen shows its
 * horizontal mirror.
 */
export function toFrame(vp: Viewport, px: number, py: number, mirror = false): { x: number; y: number } {
  const { scale, ox, oy } = fit(vp)
  if (scale <= 0) return { x: 0, y: 0 }
  const x = (px - ox) / scale
  return { x: mirror ? FRAME_W - x : x, y: (py - oy) / scale }
}

export function toScreen(vp: Viewport, x: number, y: number, mirror = false): { px: number; py: number } {
  const { scale, ox, oy } = fit(vp)
  return { px: ox + (mirror ? FRAME_W - x : x) * scale, py: oy + y * scale }
}

/** Screen rectangle of a result box (x/y are the box centre in true-image coordinates). */
export function boxToScreen(
  vp: Viewport,
  r: { x: number; y: number; width: number; height: number },
  mirror = false,
): { left: number; top: number; width: number; height: number } {
  const a = toScreen(vp, r.x - r.width / 2, r.y - r.height / 2, mirror)
  const b = toScreen(vp, r.x + r.width / 2, r.y + r.height / 2, mirror)
  return { left: Math.min(a.px, b.px), top: a.py, width: Math.abs(b.px - a.px), height: b.py - a.py }
}

/** A top-left region of the true image <-> the same region of its mirror (x' = 320 - x - w). */
export function mirrorRegion<R extends { x: number; width: number }>(r: R): R {
  return { ...r, x: FRAME_W - r.x - r.width }
}
