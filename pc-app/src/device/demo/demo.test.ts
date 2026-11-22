import { expect, test, vi } from 'vitest'
import { DemoDevice } from '../DemoDevice'
import { defaultParams, type FunctionParams, type LearnedId } from '../types'
import { createBoundsRecorder, drawGestureScene, gestureSceneAt, recognizeGestures, type DemoHand } from './gesture'
import { detectLine, drawLineScene, lineSceneAt } from './line'
import { detectMotion, drawMotionScene, motionSceneAt } from './motion'
import { detectQr, drawQrScene, qrCorners, qrSceneAt, type DemoQr } from './qr'
import { detectShapes, drawShapeScene, shapeSceneAt } from './shapes'
import { pickTarget, trackResult } from './track'
import { DemoLearnError, colorAt, matchLearned, recognize } from './recognize'
import { sceneAt, type SceneObject } from './scene'

const RED: [number, number, number] = [230, 40, 40]
const GREEN: [number, number, number] = [40, 200, 70]
const BLUE: [number, number, number] = [50, 90, 230]
const blob = (x: number, y: number, r: number, rgb: [number, number, number]): SceneObject => ({ kind: 'blob', x, y, r, rgb })
const params = () => defaultParams()

test('sceneAt is deterministic and has 4 blobs, 1 tag (id 3) and 1 face', () => {
  expect(sceneAt(12345)).toEqual(sceneAt(12345))
  expect(sceneAt(0)).not.toEqual(sceneAt(2000))
  const s = sceneAt(777)
  expect(s.filter((o) => o.kind === 'blob')).toHaveLength(4)
  expect(s.filter((o) => o.kind === 'tag').map((o) => o.tagId)).toEqual([3])
  expect(s.filter((o) => o.kind === 'face')).toHaveLength(1)
})

test('some objects leave the frame sometimes, and every centre is inside at t=0', () => {
  const inside = (o: SceneObject) => o.x - o.r >= 0 && o.x + o.r <= 320 && o.y - o.r >= 0 && o.y + o.r <= 240
  expect(sceneAt(0).every((o) => o.x > 0 && o.x < 320 && o.y > 0 && o.y < 240)).toBe(true)
  let sawOut = false
  for (let t = 0; t < 120000; t += 250) if (sceneAt(t).some((o) => !inside(o))) sawOut = true
  expect(sawOut).toBe(true)
})

test('objects are spread out: at t=0 no two centres within 40 px; face and tag boxes never intersect', () => {
  const s0 = sceneAt(0)
  for (let i = 0; i < s0.length; i++)
    for (let j = i + 1; j < s0.length; j++) expect(Math.hypot(s0[i].x - s0[j].x, s0[i].y - s0[j].y)).toBeGreaterThanOrEqual(40)
  for (let t = 0; t <= 20000; t += 50) {
    const o = sceneAt(t)
    const tag = o.find((x) => x.kind === 'tag')!
    const face = o.find((x) => x.kind === 'face')!
    const apart = Math.abs(tag.x - face.x) >= tag.r + face.r || Math.abs(tag.y - face.y) >= tag.r + face.r
    expect(apart, `t=${t}`).toBe(true)
  }
})

test('a learned colour reports its id, other blobs report 0', () => {
  const objs = [blob(60, 60, 20, RED), blob(160, 60, 20, GREEN), blob(260, 60, 20, BLUE)]
  const learned: LearnedId[] = [{ id: 5, name: '', color: GREEN }]
  const r = recognize(1, objs, learned, params())
  expect(r).toHaveLength(3)
  expect(r.find((x) => x.x === 160)!.id).toBe(5)
  expect(r.filter((x) => x.id === 0)).toHaveLength(2)
  expect(r.every((x) => x.kind === 'color')).toBe(true)
})

test('results sorted by area, capped by maxResults, small ones dropped by minArea', () => {
  const objs = [blob(60, 60, 14, RED), blob(160, 60, 30, GREEN), blob(260, 60, 20, BLUE)]
  const p = params()
  const all = recognize(1, objs, [], p)
  expect(all.map((x) => x.x)).toEqual([160, 260, 60])
  p.color.maxResults = 2
  expect(recognize(1, objs, [], p)).toHaveLength(2)
  p.color.maxResults = 4
  p.color.minArea = 800 // r=20 -> ~1256, r=14 -> ~615
  expect(recognize(1, objs, [], p).map((x) => x.x)).toEqual([160, 260])
})

test('bounding box is clipped to the frame; fully outside is not reported', () => {
  const r = recognize(1, [blob(5, 120, 20, RED), blob(-100, 50, 20, GREEN)], [], params())
  expect(r).toHaveLength(1)
  expect(r[0].width).toBe(25)
  expect(r[0].x).toBe(13) // centre of the clipped box 0..25, rounded
  expect(r[0].height).toBe(40)
})

test('tolerance widens the hue window', () => {
  const orange = blob(100, 100, 20, [230, 120, 40])
  const learned: LearnedId[] = [{ id: 1, name: '', color: RED }]
  const p = params()
  p.color.tolerance = 5
  expect(recognize(1, [orange], learned, p)[0].id).toBe(0)
  p.color.tolerance = 60
  expect(recognize(1, [orange], learned, p)[0].id).toBe(1)
})

test('tag and face are only reported for their own function', () => {
  const objs = sceneAt(0)
  expect(recognize(2, objs, [], params()).map((x) => [x.kind, x.id])).toEqual([['tag', 3]])
  expect(recognize(2, objs, [], params())[0].confidence).toBeUndefined()
  expect(recognize(3, objs, [], params()).every((x) => x.kind === 'face')).toBe(true)
  expect(recognize(1, objs, [], params()).every((x) => x.kind === 'color')).toBe(true)
})

test('face threshold filters by confidence and maxResults caps', () => {
  const objs: SceneObject[] = [{ kind: 'face', x: 100, y: 100, r: 26 }]
  const p = params()
  p.face.threshold = 0
  const r = recognize(3, objs, [], p)
  expect(r).toHaveLength(1)
  expect(r[0].confidence).toBeGreaterThanOrEqual(70)
  expect(r[0].confidence).toBeLessThanOrEqual(98)
  p.face.threshold = 99
  expect(recognize(3, objs, [], p)).toHaveLength(0)
  p.face.threshold = 0
  p.face.maxResults = 1
  expect(recognize(3, [...objs, { kind: 'face', x: 200, y: 100, r: 26 }], [], p)).toHaveLength(1)
})

test('colorAt returns the blob colour in the region, null on background', () => {
  const objs = [blob(100, 100, 20, RED), blob(220, 100, 20, BLUE)]
  expect(colorAt(objs, { x: 95, y: 95, width: 10, height: 10 })).toEqual(RED)
  expect(colorAt(objs, { x: 0, y: 0, width: 10, height: 10 })).toBeNull()
  expect(colorAt(objs, { x: 90, y: 90, width: 140, height: 20 })).not.toBeNull()
})

test('matchLearned finds an existing colour within tolerance', () => {
  const learned: LearnedId[] = [{ id: 2, name: '', color: RED }]
  expect(matchLearned([232, 45, 38], learned, 30)?.id).toBe(2)
  expect(matchLearned(BLUE, learned, 30)).toBeNull()
})

test('DemoLearnError carries an i18n-friendly code', () => {
  const e = new DemoLearnError('full')
  expect(e).toBeInstanceOf(Error)
  expect(e.code).toBe('full')
  expect(e.message).toBe('full')
})

test('hue wraparound: red near 0 degrees matches red near 350 degrees', () => {
  const learned: LearnedId[] = [{ id: 1, name: '', color: [230, 40, 36] }] // hue ~2
  expect(matchLearned([230, 40, 70], learned, 30)?.id).toBe(1) // hue ~350
  expect(matchLearned([230, 40, 120], learned, 30)).toBeNull() // hue ~337, outside +-18
})

test('low-saturation colours never match', () => {
  const grey: LearnedId[] = [{ id: 1, name: '', color: [128, 128, 128] }]
  expect(matchLearned([130, 126, 128], grey, 100)).toBeNull()
  expect(matchLearned([128, 128, 128], [{ id: 1, name: '', color: RED }], 100)).toBeNull()
})

test('minArea uses the visible area of a partially visible blob', () => {
  const p = params()
  p.color.minArea = 800
  const half = blob(0, 120, 20, RED) // half of a 1256 px^2 disc is visible (~628)
  expect(recognize(1, [half], [], p)).toHaveLength(0)
  expect(recognize(1, [blob(20, 120, 20, RED)], [], p)).toHaveLength(1)
})

test('demo blobs rarely overlap each other (< 10% of samples over 20 s)', () => {
  let overlap = 0
  let n = 0
  for (let t = 0; t < 20000; t += 50) {
    n++
    const b = sceneAt(t).filter((o) => o.kind === 'blob')
    let hit = false
    for (let i = 0; i < b.length; i++)
      for (let j = i + 1; j < b.length; j++) if (Math.hypot(b[i].x - b[j].x, b[i].y - b[j].y) < b[i].r + b[j].r) hit = true
    if (hit) overlap++
  }
  expect(overlap / n).toBeLessThan(0.1)
})

test('demo blobs still sometimes leave the frame', () => {
  let out = 0
  for (let t = 0; t < 40000; t += 50) if (sceneAt(t).some((o) => o.kind === 'blob' && (o.x - o.r < 0 || o.x + o.r > 320))) out++
  expect(out).toBeGreaterThan(0)
})

// ---- M8a: gesture demo scene ----

const hand = (o: Partial<DemoHand>): DemoHand => ({ x: 100, y: 100, size: 80, gesture: 6, handScore: 90, gestureScore: 90, ...o })
const gp = (o: Partial<FunctionParams['gesture']> = {}): FunctionParams['gesture'] => ({ ...defaultParams().gesture, ...o })

test('gestureSceneAt cycles all gestures', () => {
  const seen = new Set<number>()
  for (let t = 100; t <= 15100; t += 1500) for (const h of gestureSceneAt(t)) seen.add(h.gesture)
  expect([...seen].sort((a, b) => a - b)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9])
})

test('gestureSceneAt is deterministic, stays inside the frame, and shows a second hand part of the time', () => {
  expect(gestureSceneAt(4321)).toEqual(gestureSceneAt(4321))
  let one = 0
  let two = 0
  for (let t = 0; t < 24000; t += 100) {
    const hs = gestureSceneAt(t)
    if (hs.length === 1) one++
    else if (hs.length === 2) two++
    else throw new Error(`${hs.length} hands`)
    for (const h of hs) {
      expect(h.x - h.size / 2).toBeGreaterThanOrEqual(0)
      expect(h.x + h.size / 2).toBeLessThanOrEqual(320)
      expect(h.y - h.size / 2).toBeGreaterThanOrEqual(0)
      expect(h.y + h.size / 2).toBeLessThanOrEqual(240)
    }
  }
  expect(one).toBeGreaterThan(0)
  expect(two).toBeGreaterThan(0)
})

test('recognizeGestures applies thresholds', () => {
  const hands = [
    hand({ x: 50, handScore: 30 }), // below hand threshold: gone
    hand({ x: 200, handScore: 77, gestureScore: 50 }), // below gesture threshold: id 0, confidence = handScore
    hand({ x: 250, gesture: 7, handScore: 90, gestureScore: 88 }),
  ]
  const out = recognizeGestures(hands, gp({ handThreshold: 40, gestureThreshold: 60, maxHands: 2 }))
  expect(out).toEqual([
    { id: 7, kind: 'gesture', x: 250, y: 100, width: 80, height: 80, confidence: 88 },
    { id: 0, kind: 'gesture', x: 200, y: 100, width: 80, height: 80, confidence: 77 },
  ])
  expect(recognizeGestures(hands, gp({ handThreshold: 95 }))).toEqual([])
})

test('recognizeGestures keeps maxHands by hand score', () => {
  const hands = [hand({ x: 50, handScore: 60 }), hand({ x: 150, handScore: 95 }), hand({ x: 250, handScore: 80 })]
  expect(recognizeGestures(hands, gp({ maxHands: 1 })).map((r) => r.x)).toEqual([150])
  expect(recognizeGestures(hands, gp({ maxHands: 2 })).map((r) => r.x)).toEqual([150, 250])
})

test('the gesture box encloses the drawn hand, for every pose and size', () => {
  for (let gesture = 0; gesture <= 9; gesture++) {
    for (const size of [90, 60]) {
      const h = hand({ x: 140, y: 151, size, gesture })
      const rec = createBoundsRecorder()
      drawGestureScene(rec.ctx, [h])
      const b = rec.bounds()
      const [box] = recognizeGestures([h], gp())
      const l = box.x - box.width / 2
      const r = box.x + box.width / 2
      const t = box.y - box.height / 2
      const bt = box.y + box.height / 2
      expect([gesture, b.minX >= l, b.maxX <= r, b.minY >= t, b.maxY <= bt]).toEqual([gesture, true, true, true, true])
    }
  }
})

// ---- M6: line scene, line detection, target picking ----

const lineParams = () => defaultParams().line
const straight = (slope: number, crossY: number | null = null) => ({ path: (y: number) => 120 + slope * y, crossY, width: 14 })

test('lineSceneAt is deterministic and the line stays inside the frame', () => {
  const a = lineSceneAt(1234)
  const b = lineSceneAt(1234)
  expect([a.crossY, a.width, a.path(100), a.path(200)]).toEqual([b.crossY, b.width, b.path(100), b.path(200)])
  let sawCross = false
  let sawNoCross = false
  for (let t = 0; t < 60000; t += 250) {
    const s = lineSceneAt(t)
    for (let y = 0; y <= 240; y += 8) expect(s.path(y)).toBeGreaterThan(s.width)
    for (let y = 0; y <= 240; y += 8) expect(s.path(y)).toBeLessThan(320 - s.width)
    if (s.crossY === null) sawNoCross = true
    else sawCross = true
  }
  expect(sawCross && sawNoCross).toBe(true)
})

test('detectLine on a straight path matches the analytic offset and angle', () => {
  const r = detectLine(straight(0.25), lineParams())
  expect(r).toHaveLength(1)
  const l = r[0].line!
  // default ROI 60% of 240 -> y 96..240, 8 bands of 18 px; the bottom band centre is y = 231
  expect(r[0]).toMatchObject({ id: 1, kind: 'line', confidence: 100 })
  expect(Math.abs(l.angleDeg - -Math.atan(0.25) * (180 / Math.PI))).toBeLessThanOrEqual(1)
  expect(Math.abs(l.offsetX - (120 + 0.25 * 231 - 160))).toBeLessThanOrEqual(1)
  expect(l.start[1]).toBeGreaterThan(l.end[1]) // start is the bottom end
  expect(l.branch).toBe(false)
  expect(l.cross).toBe(false)
  expect(r[0].width).toBe(Math.abs(l.end[0] - l.start[0]) + 1)
  expect(r[0].height).toBe(Math.abs(l.start[1] - l.end[1]) + 1)
})

test('a vertical line right of centre gives angle 0 and a positive offset', () => {
  const l = detectLine({ path: () => 200, crossY: null, width: 14 }, lineParams())[0].line!
  expect(l.angleDeg).toBeCloseTo(0, 1)
  expect(l.offsetX).toBe(40)
})

test('a crossing bar sets cross and drops the bands it covers', () => {
  const r = detectLine(straight(0, 150), lineParams())
  expect(r[0].line!.cross).toBe(true)
  expect(r[0].confidence).toBeLessThan(100)
})

test('detectLine returns nothing for a light line on a dark mode board, too few bands, or too wide a line', () => {
  expect(detectLine(straight(0), { ...lineParams(), mode: 'light' })).toEqual([])
  expect(detectLine(straight(0), { ...lineParams(), roiPercent: 20, bands: 4, minWidth: 30 })).toEqual([]) // line is 14 wide
  expect(detectLine({ path: () => 160, crossY: null, width: 100 }, lineParams())).toEqual([]) // wider than max_width
})

test('drawLineScene paints without throwing', () => {
  const calls: string[] = []
  const ctx = new Proxy({}, { get: (_t, k) => (k === 'createLinearGradient' ? () => ({ addColorStop() {} }) : () => void calls.push(String(k))), set: () => true })
  drawLineScene(ctx as unknown as CanvasRenderingContext2D, 1000)
  expect(calls.length).toBeGreaterThan(0)
})

test('pickTarget picks the object that overlaps the region at least 30%, else null', () => {
  const objs = [blob(60, 60, 20, RED), blob(160, 60, 20, GREEN)]
  expect(pickTarget(objs, { x: 50, y: 50, width: 20, height: 20 })).toBe(0)
  expect(pickTarget(objs, { x: 140, y: 40, width: 40, height: 40 })).toBe(1)
  expect(pickTarget(objs, { x: 220, y: 150, width: 40, height: 40 })).toBeNull()
  expect(pickTarget(objs, { x: 79, y: 79, width: 30, height: 30 })).toBeNull() // only a corner touches
})

test('trackResult: tracked blob off-frame yields no result; a partly hidden one matches worse; below threshold is lost', () => {
  const p = defaultParams().track
  expect(trackResult(blob(-40, 100, 20, RED), p)).toEqual([])
  expect(trackResult(blob(100, 300, 20, RED), p)).toEqual([])
  const full = trackResult(blob(100, 100, 20, RED), p)[0]
  expect(full).toMatchObject({ id: 1, kind: 'track', x: 100, y: 100, width: 40, height: 40 })
  const half = trackResult(blob(0, 100, 20, RED), p)[0]
  expect(half.confidence!).toBeLessThan(full.confidence!)
  expect(trackResult(blob(0, 100, 20, RED), { ...p, threshold: 90 })).toEqual([])
})

// ---- M7: QR code, shapes and motion scenes ----

/** A canvas stand-in that records the calls and accepts every property. */
function recordingCtx() {
  const calls: string[] = []
  const ctx = new Proxy({}, { get: (_t, k) => (k === 'createLinearGradient' ? () => ({ addColorStop() {} }) : () => void calls.push(String(k))), set: () => true })
  return { ctx: ctx as unknown as CanvasRenderingContext2D, calls }
}

test('qrSceneAt is deterministic', () => {
  expect(qrSceneAt(4321)).toEqual(qrSceneAt(4321))
  expect(qrSceneAt(0)).not.toEqual(qrSceneAt(3000))
  expect(qrSceneAt(0).map((c) => c.text)).toEqual(['MXCam', 'LEFT'])
  // the cards stay whole inside the frame and never overlap, so both are always reported
  for (let t = 0; t < 60000; t += 250) {
    const cards = qrSceneAt(t)
    for (const c of cards) for (const [x, y] of qrCorners(c)) expect(x >= 0 && x < 320 && y >= 0 && y < 240, `t=${t}`).toBe(true)
    expect(detectQr(cards, [])).toHaveLength(2)
  }
})

test('detectQr returns corners and learned ids', () => {
  const card: DemoQr = { cx: 100, cy: 80, size: 40, angleDeg: 0, text: 'LEFT' }
  const learned: LearnedId[] = [{ id: 3, name: '', text: 'LEFT' }]
  const [r] = detectQr([card, { cx: 220, cy: 150, size: 60, angleDeg: 90, text: 'MXCam' }], learned).filter((x) => x.text === 'LEFT')
  expect(r).toMatchObject({ id: 3, kind: 'qr', x: 100, y: 80, width: 41, height: 41, rotationDeg: 0, text: 'LEFT', truncated: false })
  expect(r.confidence).toBeUndefined()
  expect(r.corners).toEqual([[80, 60], [120, 60], [120, 100], [80, 100]]) // top-left, top-right, bottom-right, bottom-left
  // turned 90 degrees clockwise: the code's top-left corner is now top right on screen, rotation +90
  const [turned] = detectQr([{ ...card, angleDeg: 90 }], [])
  expect(turned.id).toBe(0)
  expect(turned.rotationDeg).toBe(90)
  expect(turned.corners![0]).toEqual([120, 60])
  // largest first, at most 4, and a code cut by the frame edge is not decoded
  const many = [10, 20, 30, 40, 50].map((size, i): DemoQr => ({ cx: 30 + i * 60, cy: 120, size, angleDeg: 0, text: `c${size}` }))
  expect(detectQr(many, []).map((x) => x.text)).toEqual(['c50', 'c40', 'c30', 'c20'])
  expect(detectQr([{ ...card, cx: 5 }], [])).toEqual([])
})

test('demo learn(6) on a card learns its text', async () => {
  const d = new DemoDevice(() => 0)
  await d.setFunction(6)
  const [big, small] = detectQr(qrSceneAt(0), [])
  const box = (r: { x: number; y: number }) => ({ x: r.x - 10, y: r.y - 10, width: 20, height: 20 })
  expect(await d.learn(6, box(big))).toBe(1)
  expect(await d.learn(6, box(small))).toBe(2)
  expect(await d.learn(6, box(big))).toBe(1) // same content: same ID
  expect(d.snapshot().learned[6]).toEqual([
    { id: 1, name: '', text: 'MXCam' },
    { id: 2, name: '', text: 'LEFT' },
  ])
  expect(d.snapshot().dirty).toBe(true)
  // the whole frame: the larger card wins
  await d.forgetAll(6)
  expect(await d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).toBe(1)
  expect(d.snapshot().learned[6][0].text).toBe('MXCam')
  // names need a learned ID
  await d.setName(6, 1, '起點')
  await d.setName(6, 5, 'ghost')
  expect(d.snapshot().learned[6]).toEqual([{ id: 1, name: '起點', text: 'MXCam' }])
  d.dispose()
})

test('demo learn(6) on empty floor is no-code', async () => {
  const d = new DemoDevice(() => 0)
  await d.setFunction(6)
  await expect(d.learn(6, { x: 0, y: 0, width: 8, height: 8 })).rejects.toMatchObject({ code: 'no-code' })
  expect(d.snapshot().learned[6]).toEqual([])
  d.dispose()
})

test('demo learn(6) is full after 8 contents', async () => {
  const d = new DemoDevice(() => 0)
  await d.importConfig(
    JSON.stringify({
      version: 1,
      learned: { 1: [], 2: [], 3: [], 6: [1, 2, 3, 4, 5, 6, 7, 8].map((id) => ({ id, name: '', text: `other${id}` })) },
      params: d.snapshot().params,
      system: d.snapshot().system,
    }),
  )
  await d.setFunction(6)
  await expect(d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).rejects.toMatchObject({ code: 'full' })
  d.dispose()
})

test('drawQrScene, drawShapeScene and drawMotionScene paint without throwing', () => {
  const draws = [
    (c: CanvasRenderingContext2D) => drawQrScene(c, qrSceneAt(1000)),
    (c: CanvasRenderingContext2D) => drawShapeScene(c, shapeSceneAt(1000)),
    (c: CanvasRenderingContext2D) => drawMotionScene(c, motionSceneAt(1000)),
  ]
  for (const draw of draws) {
    const { ctx, calls } = recordingCtx()
    draw(ctx)
    expect(calls.length).toBeGreaterThan(0)
  }
})

const shapeParams = (o: Partial<FunctionParams['shape']> = {}): FunctionParams['shape'] => ({ ...defaultParams().shape, ...o })

test('detectShapes finds the four shapes', () => {
  expect(shapeSceneAt(2500)).toEqual(shapeSceneAt(2500))
  for (let t = 0; t < 40000; t += 500) {
    const found = detectShapes(shapeSceneAt(t), shapeParams())
    expect(found.map((r) => r.id).sort(), `t=${t}`).toEqual([1, 2, 3, 4])
    expect(found.every((r) => r.kind === 'shape' && r.confidence! >= 80 && r.confidence! <= 100)).toBe(true)
  }
  const two = detectShapes(shapeSceneAt(0), shapeParams({ maxResults: 2 }))
  expect(two).toHaveLength(2)
  expect(detectShapes(shapeSceneAt(0), shapeParams({ mode: 'light' }))).toEqual([]) // the scene has dark shapes
  expect(detectShapes(shapeSceneAt(0), shapeParams({ minSize: 240 }))).toEqual([])
  // a shape touching the frame edge is not reported
  expect(detectShapes([{ id: 3, cx: 10, cy: 100, size: 40, angleDeg: 0 }], shapeParams())).toEqual([])
})

const motionParams = (o: Partial<FunctionParams['motion']> = {}): FunctionParams['motion'] => ({ ...defaultParams().motion, ...o })

test('detectMotion reports only while moving', () => {
  expect(motionSceneAt(1234)).toEqual(motionSceneAt(1234))
  let moving = 0
  let still = 0
  for (let t = 0; t < 20000; t += 100) {
    const s = motionSceneAt(t)
    const r = detectMotion(s, motionParams())
    if (s.moving) {
      moving++
      expect(r).toHaveLength(1)
      expect(r[0]).toMatchObject({ id: 0, kind: 'motion' })
      expect(Math.abs(r[0].x - s.x)).toBeLessThanOrEqual(1)
    } else {
      still++
      expect(r).toEqual([]) // nothing while the ball rests
    }
  }
  expect(moving).toBeGreaterThan(0)
  expect(still).toBeGreaterThan(0)
  const rolling = motionSceneAt(1000)
  expect(rolling.moving).toBe(true)
  expect(detectMotion(rolling, motionParams({ threshold: 100 }))).toEqual([]) // less sensitive than the ball's contrast
  expect(detectMotion(rolling, motionParams({ minSize: 200 }))).toEqual([])
})

test('demo names shape ids', async () => {
  const d = new DemoDevice(() => 0)
  await d.setName(11, 1, '圓')
  expect(d.snapshot().learned[11]).toEqual([{ id: 1, name: '圓' }])
  await expect(d.setName(11, 0, 'x')).rejects.toThrow()
  await expect(d.setName(11, 5, 'x')).rejects.toThrow()
  await expect(d.setName(12, 1, 'x')).rejects.toThrow() // motion cannot be named
  await expect(d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).rejects.toMatchObject({ code: 'no-code' }) // QR is not running
  d.dispose()
})

test('demo emits QR, shape and motion results for functions 6, 11 and 12; params are clamped', async () => {
  vi.useFakeTimers()
  let clock = 0
  const d = new DemoDevice(() => clock)
  const got: { kind: string }[][] = []
  d.onResults((r) => got.push(r))
  for (const [fn, kind] of [[6, 'qr'], [11, 'shape'], [12, 'motion']] as const) {
    await d.setFunction(fn)
    clock = 1000 // the ball rolls during the first 3 s of each 5 s
    vi.advanceTimersByTime(100)
    expect(got.at(-1)!.length, kind).toBeGreaterThan(0)
    expect(got.at(-1)!.every((r) => r.kind === kind)).toBe(true)
  }
  await d.setParams({ shape: { mode: 'light', threshold: 300, minSize: 2, maxResults: 20 }, motion: { threshold: 1, minSize: 999, maxResults: 0 } })
  expect(d.snapshot().params.shape).toEqual({ mode: 'light', threshold: 255, minSize: 16, maxResults: 8 })
  expect(d.snapshot().params.motion).toEqual({ threshold: 5, minSize: 240, maxResults: 1 })
  d.dispose()
  vi.useRealTimers()
})
