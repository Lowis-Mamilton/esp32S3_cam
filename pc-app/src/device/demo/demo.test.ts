import { expect, test } from 'vitest'
import { defaultParams, type LearnedId } from '../types'
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
