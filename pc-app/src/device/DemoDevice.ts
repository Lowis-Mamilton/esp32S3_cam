import { parseConfig } from './config'
import { drawGestureScene, gestureSceneAt, recognizeGestures } from './demo/gesture'
import { detectLine, drawLineScene, lineSceneAt } from './demo/line'
import { detectMotion, drawMotionScene, motionSceneAt } from './demo/motion'
import { detectQr, drawQrScene, qrSceneAt, type DemoQr } from './demo/qr'
import { colorAt, matchLearned, recognize } from './demo/recognize'
import { drawScene, sceneAt, type SceneObject } from './demo/scene'
import { detectShapes, drawShapeScene, shapeSceneAt } from './demo/shapes'
import { objectColor, pickTarget, trackResult } from './demo/track'
import {
  Emitter,
  LearnError,
  NAME_MAX_BYTES,
  clampLineParams,
  clampMotionParams,
  clampShapeParams,
  clampTrackParams,
  defaultParams,
  defaultSystem,
  emptyLearned,
  nameBytes,
  type Device,
  type DeviceFunctionId,
  type DeviceInfo,
  type DeviceSnapshot,
  type FrameSource,
  type FunctionId,
  type FunctionParams,
  type ImportOutcome,
  type LearnableFn,
  type Result,
  type SystemConfig,
} from './types'

const DEMO_FNS: DeviceFunctionId[] = [1, 2, 3, 4, 5, 6, 8, 11, 12]
export const DEMO_INFO: DeviceInfo = {
  protocolVersion: 1,
  firmwareVersion: 'DEMO',
  firmwareGit: 'demo',
  serial: 'DE:MO:00:00:00:01',
  frameWidth: 320,
  frameHeight: 240,
  maxResults: 8,
  functionIds: DEMO_FNS,
}
/** QR IDs 1-8, like the firmware. */
const MAX_QR_IDS = 8

const FRAME_INTERVAL_MS = 100

const clampN = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, Math.round(v)))
/** Like the firmware (protocol.md section 10). */
const clampGesture = (g: FunctionParams['gesture']): FunctionParams['gesture'] => ({
  handThreshold: clampN(g.handThreshold, 10, 95),
  gestureThreshold: clampN(g.gestureThreshold, 0, 100),
  maxHands: clampN(g.maxHands, 1, 2) as 1 | 2,
})

/** Browser-only device: simulated scene and recognition, config kept in memory. `now` is injectable for tests. */
export class DemoDevice implements Device {
  private readonly changes = new Emitter<[]>()
  private readonly frames = new Emitter<[FrameSource]>()
  private readonly results = new Emitter<[Result[]]>()
  private readonly stats = new Emitter<[{ fps: number; kbps: number }]>()
  private learned = emptyLearned()
  private params = defaultParams()
  private system = defaultSystem()
  private active: DeviceFunctionId | 0 = 1
  private dirty = false
  /** Scene object (index in sceneAt) the tracker follows after learn(5); null = nothing learned. */
  private trackIdx: number | null = null
  private previewPaused = false
  private snap: DeviceSnapshot | null = null
  private timer: ReturnType<typeof setInterval> | null
  private tick = 0
  private readonly t0: number
  /** The scene of the last emitted frame: what the user saw, and what learn samples. */
  private lastObjs: SceneObject[] = sceneAt(0)
  /** The QR cards of the last emitted frame (function 6): what learn(6) picks from. */
  private lastQr: DemoQr[] = qrSceneAt(0)
  private readonly now: () => number

  constructor(now: () => number = () => performance.now()) {
    this.now = now
    this.t0 = now()
    this.timer = setInterval(() => this.onTick(), FRAME_INTERVAL_MS)
  }

  private onTick() {
    const n = this.tick++
    const t = this.now() - this.t0
    const objs = sceneAt(t)
    this.lastObjs = objs
    if (this.active === 4) {
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawLineScene(ctx, t) })
      this.results.emit(detectLine(lineSceneAt(t), this.params.line))
    } else if (this.active === 5) {
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawScene(ctx, objs) })
      this.results.emit(this.trackIdx === null ? [] : trackResult(objs[this.trackIdx], this.params.track))
    } else if (this.active === 8) {
      const hands = gestureSceneAt(t)
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawGestureScene(ctx, hands) })
      this.results.emit(recognizeGestures(hands, this.params.gesture))
    } else if (this.active === 6) {
      const cards = qrSceneAt(t)
      this.lastQr = cards
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawQrScene(ctx, cards) })
      this.results.emit(detectQr(cards, this.learned[6]))
    } else if (this.active === 11) {
      const shapes = shapeSceneAt(t)
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawShapeScene(ctx, shapes) })
      this.results.emit(detectShapes(shapes, this.params.shape))
    } else if (this.active === 12) {
      const ball = motionSceneAt(t)
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawMotionScene(ctx, ball) })
      this.results.emit(detectMotion(ball, this.params.motion))
    } else {
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawScene(ctx, objs) })
      this.results.emit(this.active === 0 ? [] : recognize(this.active, objs, this.learned[this.active], this.params))
    }
    if (n % 10 === 0) this.stats.emit({ fps: 1000 / FRAME_INTERVAL_MS, kbps: 0 })
  }

  private changed() {
    this.snap = null
    this.changes.emit()
  }
  private touch() {
    this.dirty = true
    this.changed()
  }

  snapshot(): DeviceSnapshot {
    return (this.snap ??= {
      mode: 'demo',
      status: 'connected',
      info: DEMO_INFO,
      // Includes 6, 11 and 12; see DeviceSnapshot.supported for the type.
      supported: DEMO_FNS as FunctionId[],
      activeFunction: this.active,
      learned: this.learned,
      params: this.params,
      system: this.system,
      dirty: this.dirty,
      lost: false,
      configKnown: true,
      previewPaused: this.previewPaused,
      transports: [],
      transport: null,
    })
  }

  subscribe(cb: () => void) {
    return this.changes.on(cb)
  }
  onFrame(cb: (frame: FrameSource) => void) {
    return this.frames.on(cb)
  }
  onResults(cb: (results: Result[]) => void) {
    return this.results.on(cb)
  }
  onStats(cb: (s: { fps: number; kbps: number }) => void) {
    return this.stats.on(cb)
  }
  /** Nobody else controls the demo. */
  onFunctionChange() {
    return () => {}
  }

  async connect() {}
  async disconnect() {}

  async setFunction(id: DeviceFunctionId) {
    this.active = id
    this.changed()
  }

  async setPreviewPaused(paused: boolean) {
    if (paused === this.previewPaused) return
    this.previewPaused = paused
    this.changed()
  }

  async learn(fn: LearnableFn, region: { x: number; y: number; width: number; height: number }): Promise<number> {
    if (fn === 4) throw new LearnError('no-color') // the demo line is black tape: no colour to learn (use the dark mode)
    if (fn === 5) return this.learnTarget(region)
    if (fn === 6) return this.learnQr(region)
    if (this.active === 8) throw new LearnError('no-color') // gestures are fixed: nothing to learn
    const rgb = colorAt(this.lastObjs, region)
    if (!rgb) throw new LearnError('no-color')
    const list = this.learned[1]
    const same = matchLearned(rgb, list, this.params.color.tolerance)
    if (same) {
      const c = same.color
      if (c && c[0] === rgb[0] && c[1] === rgb[1] && c[2] === rgb[2]) return same.id // nothing changed
      this.learned = { ...this.learned, 1: list.map((e) => (e.id === same.id ? { ...e, color: rgb } : e)) }
      this.touch()
      return same.id
    }
    let id = 1
    while (list.some((e) => e.id === id)) id++
    if (id > 7) throw new LearnError('full')
    this.learned = { ...this.learned, 1: [...list, { id, name: '', color: rgb }].sort((a, b) => a.id - b.id) }
    this.touch()
    return id
  }
  /** Learn 5: the object under the selection becomes the one tracked target (id 1); learning again replaces it and its name. */
  private learnTarget(region: { x: number; y: number; width: number; height: number }): number {
    const idx = pickTarget(this.lastObjs, region)
    if (idx === null) throw new LearnError('no-target')
    this.trackIdx = idx
    this.learned = { ...this.learned, 5: [{ id: 1, name: '', color: objectColor(this.lastObjs[idx]) }] }
    this.touch()
    return 1
  }
  /**
   * Learn 6, like the firmware: of the cards on the last frame whose centre is in the region, the largest; its content
   * already learned -> that ID (nothing changes); otherwise the lowest free ID 1-8, or 'full'; no card -> 'no-code'.
   */
  private learnQr(region: { x: number; y: number; width: number; height: number }): number {
    if (this.active !== 6) throw new LearnError('no-code')
    const hit = detectQr(this.lastQr, [])
      .filter((r) => r.x >= region.x && r.x < region.x + region.width && r.y >= region.y && r.y < region.y + region.height)
      .sort((a, b) => b.width * b.height - a.width * a.height)[0]
    if (!hit?.text) throw new LearnError('no-code')
    const list = this.learned[6]
    const same = list.find((e) => e.text === hit.text)
    if (same) return same.id
    let id = 1
    while (list.some((e) => e.id === id)) id++
    if (id > MAX_QR_IDS) throw new LearnError('full')
    this.learned = { ...this.learned, 6: [...list, { id, name: '', text: hit.text }].sort((a, b) => a.id - b.id) }
    this.touch()
    return id
  }
  async forget(fn: DeviceFunctionId, id: number) {
    this.learned = { ...this.learned, [fn]: this.learned[fn].filter((e) => e.id !== id) }
    if (fn === 5 && this.learned[5].length === 0) this.trackIdx = null
    this.touch()
  }
  async forgetAll(fn: DeviceFunctionId) {
    this.learned = { ...this.learned, [fn]: [] }
    if (fn === 5) this.trackIdx = null
    this.touch()
  }
  async setName(fn: DeviceFunctionId, id: number, name: string) {
    if (nameBytes(name) > NAME_MAX_BYTES) throw new Error(`name longer than ${NAME_MAX_BYTES} bytes`)
    if (fn === 2 && !(Number.isInteger(id) && id >= 0 && id <= 586)) throw new Error('invalid tag id')
    if (fn === 8 && !(Number.isInteger(id) && id >= 1 && id <= 9)) throw new Error('invalid gesture id')
    if (fn === 11 && !(Number.isInteger(id) && id >= 1 && id <= 4)) throw new Error('invalid shape id')
    if (fn === 4) throw new Error('the line cannot be named')
    if (fn === 12) throw new Error('motion cannot be named')
    const list = this.learned[fn]
    const cur = list.find((e) => e.id === id)
    // Colours, QR codes and faces must already be learned; tags, gestures and shapes need no learning, so naming an
    // unseen one creates it.
    if (!cur && fn !== 2 && fn !== 8 && fn !== 11) return
    if (cur && cur.name === name) return
    const next = cur ? list.map((e) => (e.id === id ? { ...e, name } : e)) : [...list, { id, name }].sort((a, b) => a.id - b.id)
    this.learned = { ...this.learned, [fn]: next }
    this.touch()
  }
  async setParams(p: Partial<FunctionParams>) {
    const next = {
      color: { ...this.params.color, ...p.color },
      tag: { ...this.params.tag, ...p.tag },
      face: { ...this.params.face, ...p.face },
      gesture: p.gesture ? clampGesture({ ...this.params.gesture, ...p.gesture }) : this.params.gesture,
      line: p.line ? clampLineParams({ ...this.params.line, ...p.line }) : this.params.line,
      track: p.track ? clampTrackParams({ ...this.params.track, ...p.track }) : this.params.track,
      shape: p.shape ? clampShapeParams({ ...this.params.shape, ...p.shape }) : this.params.shape,
      motion: p.motion ? clampMotionParams({ ...this.params.motion, ...p.motion }) : this.params.motion,
    }
    if (JSON.stringify(next) === JSON.stringify(this.params)) return // nothing changed: stay clean
    this.params = next
    this.touch()
  }
  async setSystem(c: Partial<SystemConfig>) {
    const next = { ...this.system, ...c }
    if (JSON.stringify(next) === JSON.stringify(this.system)) return
    this.system = next
    this.touch()
  }
  async save() {
    this.dirty = false
    this.changed()
  }
  async factoryReset() {
    this.learned = emptyLearned()
    this.trackIdx = null
    this.params = defaultParams()
    this.system = defaultSystem()
    this.dirty = false
    this.changed()
  }

  exportConfig(): string {
    return JSON.stringify({ version: 1, learned: this.learned, params: this.params, system: this.system })
  }
  async importConfig(json: string): Promise<ImportOutcome> {
    const c = parseConfig(json) // throws before any state is touched
    // A tracked object is a template from the camera: a file cannot bring it back, so it has to be learned again.
    const hadTarget = c.learned[5].length > 0
    this.learned = { ...c.learned, 5: [] }
    this.trackIdx = null
    this.params = {
      ...c.params,
      gesture: c.params.gesture ?? this.params.gesture,
      line: c.params.line ?? this.params.line,
      track: c.params.track ?? this.params.track,
      shape: c.params.shape ?? this.params.shape,
      motion: c.params.motion ?? this.params.motion,
    }
    this.system = c.system
    this.touch()
    return { colorsNeedRelearn: hadTarget }
  }

  dispose() {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
    this.changes.clear()
    this.frames.clear()
    this.results.clear()
    this.stats.clear()
  }
}
