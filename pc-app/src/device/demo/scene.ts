/** Demo scene in 320x240 frame space. Pure except drawScene (the only canvas code). */
export interface SceneObject {
  kind: 'blob' | 'tag' | 'face'
  x: number
  y: number
  r: number // blob/face radius, tag half-side
  rgb?: [number, number, number]
  tagId?: number
}

export const FRAME_W = 320
export const FRAME_H = 240

interface Path {
  cx: number
  cy: number
  ax: number
  ay: number
  px: number // period in ms
  py: number
  phx: number // phase in radians
  phy: number
}

// x = cx + ax*sin(2*pi*t/px + phx), y = cy + ay*sin(2*pi*t/py + phy): a Lissajous curve.
// Each object roams its own region so the scene stays readable: the tag stays in the upper-left
// (x 30..110, y 35..85), the face in the upper-right (x 210..290, y 35..85), so their bounding boxes
// can never intersect; blobs use the lower half (centre y >= 130). Blob x amplitudes reach past the
// frame edge so blobs periodically leave it partially or fully. Phases spread the start positions.
const at = (p: Path, t: number): [number, number] => [
  p.cx + p.ax * Math.sin((2 * Math.PI * t) / p.px + p.phx),
  p.cy + p.ay * Math.sin((2 * Math.PI * t) / p.py + p.phy),
]

const BLOBS: { r: number; rgb: [number, number, number]; path: Path }[] = [
  { r: 24, rgb: [230, 40, 40], path: { cx: 70, cy: 180, ax: 85, ay: 30, px: 14000, py: 9000, phx: 0.5, phy: 0 } }, // red
  { r: 18, rgb: [40, 200, 70], path: { cx: 160, cy: 200, ax: 60, ay: 40, px: 11000, py: 7000, phx: 2, phy: 1 } }, // green
  { r: 30, rgb: [50, 90, 230], path: { cx: 250, cy: 170, ax: 95, ay: 25, px: 16000, py: 10000, phx: 4, phy: 2 } }, // blue
  { r: 14, rgb: [240, 210, 40], path: { cx: 160, cy: 150, ax: 170, ay: 20, px: 9000, py: 13000, phx: 1.2, phy: 3 } }, // yellow
]
const TAG = { half: 20, id: 3, path: { cx: 70, cy: 60, ax: 40, ay: 25, px: 30000, py: 40000, phx: 0.8, phy: 0 } }
const FACE = { r: 26, path: { cx: 250, cy: 60, ax: 40, ay: 25, px: 36000, py: 28000, phx: 3, phy: 1.5 } }

const round1 = (n: number) => Math.round(n * 10) / 10

/** Deterministic: the same time always yields the same scene. */
export function sceneAt(timeMs: number): SceneObject[] {
  const objs: SceneObject[] = BLOBS.map((b) => {
    const [x, y] = at(b.path, timeMs)
    return { kind: 'blob', x: round1(x), y: round1(y), r: b.r, rgb: b.rgb }
  })
  const [tx, ty] = at(TAG.path, timeMs)
  objs.push({ kind: 'tag', x: round1(tx), y: round1(ty), r: TAG.half, tagId: TAG.id })
  const [fx, fy] = at(FACE.path, timeMs)
  objs.push({ kind: 'face', x: round1(fx), y: round1(fy), r: FACE.r })
  return objs
}

// 5x5 pattern drawn in white on the black inner square (1 = white cell). Looks like a tag; not a decodable one.
const TAG_CELLS = ['10110', '01001', '11010', '00111', '10101']

export function drawScene(ctx: CanvasRenderingContext2D, objs: SceneObject[]): void {
  const g = ctx.createLinearGradient(0, 0, FRAME_W, FRAME_H)
  g.addColorStop(0, '#2b3340')
  g.addColorStop(1, '#46505e')
  ctx.fillStyle = g
  ctx.fillRect(0, 0, FRAME_W, FRAME_H)
  for (const o of objs) {
    if (o.kind === 'blob' && o.rgb) {
      ctx.fillStyle = `rgb(${o.rgb[0]},${o.rgb[1]},${o.rgb[2]})`
      ctx.beginPath()
      ctx.arc(o.x, o.y, o.r, 0, Math.PI * 2)
      ctx.fill()
    } else if (o.kind === 'tag') {
      const s = o.r * 2
      const x0 = o.x - o.r
      const y0 = o.y - o.r
      ctx.fillStyle = '#fff' // white border
      ctx.fillRect(x0, y0, s, s)
      const inner = s - 8
      ctx.fillStyle = '#000'
      ctx.fillRect(x0 + 4, y0 + 4, inner, inner)
      const cell = (inner - 8) / 5
      ctx.fillStyle = '#fff'
      TAG_CELLS.forEach((row, j) =>
        [...row].forEach((c, i) => {
          if (c === '1') ctx.fillRect(x0 + 8 + i * cell, y0 + 8 + j * cell, cell, cell)
        }),
      )
    } else if (o.kind === 'face') {
      ctx.fillStyle = '#f0be96'
      ctx.beginPath()
      ctx.arc(o.x, o.y, o.r, 0, Math.PI * 2)
      ctx.fill()
      ctx.fillStyle = '#222'
      ctx.beginPath()
      ctx.arc(o.x - o.r * 0.35, o.y - o.r * 0.2, o.r * 0.1, 0, Math.PI * 2)
      ctx.arc(o.x + o.r * 0.35, o.y - o.r * 0.2, o.r * 0.1, 0, Math.PI * 2)
      ctx.fill()
      ctx.strokeStyle = '#222'
      ctx.lineWidth = 2
      ctx.beginPath()
      ctx.arc(o.x, o.y + o.r * 0.1, o.r * 0.45, 0.2 * Math.PI, 0.8 * Math.PI)
      ctx.stroke()
    }
  }
}
