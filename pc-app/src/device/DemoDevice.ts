import { parseConfig } from './config'
import { drawGestureScene, gestureSceneAt, recognizeGestures } from './demo/gesture'
import { colorAt, matchLearned, recognize } from './demo/recognize'
import { drawScene, sceneAt, type SceneObject } from './demo/scene'
import {
  Emitter,
  LearnError,
  NAME_MAX_BYTES,
  defaultParams,
  defaultSystem,
  emptyLearned,
  nameBytes,
  type Device,
  type DeviceInfo,
  type DeviceSnapshot,
  type FrameSource,
  type FunctionId,
  type FunctionParams,
  type ImportOutcome,
  type Result,
  type SystemConfig,
} from './types'

export const DEMO_INFO: DeviceInfo = {
  protocolVersion: 1,
  firmwareVersion: 'DEMO',
  firmwareGit: 'demo',
  serial: 'DE:MO:00:00:00:01',
  frameWidth: 320,
  frameHeight: 240,
  maxResults: 8,
  functionIds: [1, 2, 3, 8],
}

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
  private active: FunctionId | 0 = 1
  private dirty = false
  private previewPaused = false
  private snap: DeviceSnapshot | null = null
  private timer: ReturnType<typeof setInterval> | null
  private tick = 0
  private readonly t0: number
  /** The scene of the last emitted frame: what the user saw, and what learnColor samples. */
  private lastObjs: SceneObject[] = sceneAt(0)
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
    if (this.active === 8) {
      const hands = gestureSceneAt(t)
      if (!this.previewPaused) this.frames.emit({ kind: 'canvas', draw: (ctx) => drawGestureScene(ctx, hands) })
      this.results.emit(recognizeGestures(hands, this.params.gesture))
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
      supported: [1, 2, 3, 8],
      activeFunction: this.active,
      learned: this.learned,
      params: this.params,
      system: this.system,
      dirty: this.dirty,
      lost: false,
      configKnown: true,
      previewPaused: this.previewPaused,
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

  async setFunction(id: FunctionId) {
    this.active = id
    this.changed()
  }

  async setPreviewPaused(paused: boolean) {
    if (paused === this.previewPaused) return
    this.previewPaused = paused
    this.changed()
  }

  async learnColor(region: { x: number; y: number; width: number; height: number }): Promise<number> {
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
  async forget(fn: FunctionId, id: number) {
    this.learned = { ...this.learned, [fn]: this.learned[fn].filter((e) => e.id !== id) }
    this.touch()
  }
  async forgetAll(fn: FunctionId) {
    this.learned = { ...this.learned, [fn]: [] }
    this.touch()
  }
  async setName(fn: FunctionId, id: number, name: string) {
    if (nameBytes(name) > NAME_MAX_BYTES) throw new Error(`name longer than ${NAME_MAX_BYTES} bytes`)
    if (fn === 2 && !(Number.isInteger(id) && id >= 0 && id <= 586)) throw new Error('invalid tag id')
    if (fn === 8 && !(Number.isInteger(id) && id >= 1 && id <= 9)) throw new Error('invalid gesture id')
    const list = this.learned[fn]
    const cur = list.find((e) => e.id === id)
    // Colours and faces must already be learned; tags and gestures need no learning, so naming an unseen one creates it.
    if (!cur && fn !== 2 && fn !== 8) return
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
    this.learned = c.learned
    this.params = c.params
    this.system = c.system
    this.touch()
    return { colorsNeedRelearn: false }
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
