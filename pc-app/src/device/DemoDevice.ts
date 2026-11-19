import { parseConfig } from './config'
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

/** Browser-only device: draws a placeholder scene, keeps config in memory. Task 3 adds the real scene. */
export class DemoDevice implements Device {
  private readonly changes = new Emitter<[]>()
  private readonly frames = new Emitter<[FrameSource]>()
  private readonly results = new Emitter<[Result[]]>()
  private readonly stats = new Emitter<[{ fps: number; kbps: number }]>()
  private learned = emptyLearned()
  private params = defaultParams()
  private system = defaultSystem()
  private active: FunctionId | 0 = 0
  private dirty = false
  private snap: DeviceSnapshot | null = null
  private timer: ReturnType<typeof setInterval> | null
  private tick = 0

  constructor() {
    this.timer = setInterval(() => this.onTick(), FRAME_INTERVAL_MS)
  }

  private onTick() {
    const n = this.tick++
    const w = DEMO_INFO.frameWidth
    const h = DEMO_INFO.frameHeight
    this.frames.emit({
      kind: 'canvas',
      draw: (ctx) => {
        const g = ctx.createLinearGradient(0, 0, w, h)
        g.addColorStop(0, '#1f2937')
        g.addColorStop(1, '#374151')
        ctx.fillStyle = g
        ctx.fillRect(0, 0, w, h)
        ctx.fillStyle = '#f59e0b'
        ctx.font = 'bold 20px sans-serif'
        ctx.fillText('DEMO', 12, 28)
        ctx.beginPath()
        ctx.arc(60 + ((n * 4) % (w - 120)), h / 2, 16, 0, Math.PI * 2)
        ctx.fill()
      },
    })
    this.results.emit([])
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

  // Placeholder: Task 3 samples the demo scene colour instead of using a fixed grey.
  async learnColor(_region: { x: number; y: number; width: number; height: number }): Promise<number> {
    const list = this.learned[1]
    const id = list.reduce((m, e) => Math.max(m, e.id), 0) + 1
    this.learned = { ...this.learned, 1: [...list, { id, name: `ID ${id}`, color: [128, 128, 128] }] }
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
