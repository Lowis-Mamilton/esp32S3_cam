import { parseConfig } from './config'
import { DemoLearnError, colorAt, matchLearned, recognize } from './demo/recognize'
import { drawScene, sceneAt, type SceneObject } from './demo/scene'
import {
  Emitter,
  defaultParams,
  defaultSystem,
  emptyLearned,
  type Device,
  type DeviceInfo,
  type DeviceSnapshot,
  type FrameSource,
  type FunctionId,
  type FunctionParams,
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
  functionIds: [1, 2, 3],
}

const FRAME_INTERVAL_MS = 100

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
    const objs = sceneAt(this.now() - this.t0)
    this.lastObjs = objs
    this.frames.emit({ kind: 'canvas', draw: (ctx) => drawScene(ctx, objs) })
    this.results.emit(this.active === 0 ? [] : recognize(this.active, objs, this.learned[this.active], this.params))
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
      supported: [1, 2, 3],
      activeFunction: this.active,
      learned: this.learned,
      params: this.params,
      system: this.system,
      dirty: this.dirty,
      lost: false,
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

  async connect() {}
  async disconnect() {}

  async setFunction(id: FunctionId) {
    this.active = id
    this.changed()
  }

  async learnColor(region: { x: number; y: number; width: number; height: number }): Promise<number> {
    const rgb = colorAt(this.lastObjs, region)
    if (!rgb) throw new DemoLearnError('no-color')
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
    if (id > 7) throw new DemoLearnError('full')
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
    this.learned = { ...this.learned, [fn]: this.learned[fn].map((e) => (e.id === id ? { ...e, name } : e)) }
    this.touch()
  }
  async setParams(p: Partial<FunctionParams>) {
    this.params = {
      color: { ...this.params.color, ...p.color },
      tag: { ...this.params.tag, ...p.tag },
      face: { ...this.params.face, ...p.face },
    }
    this.touch()
  }
  async setSystem(c: Partial<SystemConfig>) {
    this.system = { ...this.system, ...c }
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
  async importConfig(json: string) {
    const c = parseConfig(json) // throws before any state is touched
    this.learned = c.learned
    this.params = c.params
    this.system = c.system
    this.touch()
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
