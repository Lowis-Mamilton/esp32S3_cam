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

/** Screen (relative to the stage box) to frame coordinates. Not clamped: callers decide. */
export function toFrame(vp: Viewport, px: number, py: number): { x: number; y: number } {
  const { scale, ox, oy } = fit(vp)
  if (scale <= 0) return { x: 0, y: 0 }
  return { x: (px - ox) / scale, y: (py - oy) / scale }
}

export function toScreen(vp: Viewport, x: number, y: number): { px: number; py: number } {
  const { scale, ox, oy } = fit(vp)
  return { px: ox + x * scale, py: oy + y * scale }
}
