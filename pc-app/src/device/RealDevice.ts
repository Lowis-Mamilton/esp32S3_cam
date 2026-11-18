import { MxcamConnection, type ConnectionStatus } from '../serial/connection'
import {
  Emitter,
  NotSupportedError,
  defaultParams,
  defaultSystem,
  emptyLearned,
  toFunctionIds,
  type Device,
  type DeviceInfo,
  type DeviceSnapshot,
  type FrameSource,
  type Result,
} from './types'

/** Wraps MxcamConnection. Firmware M1 has no learn/params/system commands, so those throw. */
export class RealDevice implements Device {
  private readonly conn: MxcamConnection
  private readonly changes = new Emitter<[]>()
  private readonly frames = new Emitter<[FrameSource]>()
  private readonly results = new Emitter<[Result[]]>()
  private readonly stats = new Emitter<[{ fps: number; kbps: number }]>()
  private status: ConnectionStatus
  private info: DeviceInfo | undefined
  private lost = false
  private userClosed = false
  private snap: DeviceSnapshot | null = null
  private readonly auto: ReturnType<typeof setTimeout>
  private disposed = false

  constructor(serial: Serial | undefined) {
    this.status = serial ? 'disconnected' : 'unsupported'
    this.conn = new MxcamConnection(serial, {
      status: (s) => {
        this.status = s
        if (s === 'connecting') {
          this.userClosed = false
          this.lost = false
        } else if (s === 'disconnected') {
          this.lost = !this.userClosed
          this.info = undefined
        }
        this.changed()
      },
      frame: (f) => this.frames.emit({ kind: 'jpeg', jpeg: f.jpeg }),
      stats: (s) => this.stats.emit(s),
      info: (i) => {
        this.info = i
        this.changed()
      },
    })
    // Deferred so StrictMode's mount/unmount/mount does not race two opens of one port.
    this.auto = setTimeout(() => void this.conn.autoConnect(), 0)
  }

  private changed() {
    this.snap = null
    this.changes.emit()
  }

  snapshot(): DeviceSnapshot {
    return (this.snap ??= {
      mode: 'real',
      status: this.status,
      info: this.info,
      supported: toFunctionIds(this.info?.functionIds ?? []),
      activeFunction: 0,
      learned: emptyLearned(),
      params: defaultParams(),
      system: defaultSystem(),
      dirty: false,
      lost: this.lost,
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

  async connect() {
    await this.conn.connect()
  }
  async disconnect() {
    this.userClosed = true
    await this.conn.disconnect()
  }

  private unsupported(): Promise<never> {
    return Promise.reject(new NotSupportedError())
  }
  setFunction(): Promise<void> {
    return this.unsupported()
  }
  learnColor(): Promise<number> {
    return this.unsupported()
  }
  forget(): Promise<void> {
    return this.unsupported()
  }
  forgetAll(): Promise<void> {
    return this.unsupported()
  }
  setName(): Promise<void> {
    return this.unsupported()
  }
  setParams(): Promise<void> {
    return this.unsupported()
  }
  setSystem(): Promise<void> {
    return this.unsupported()
  }
  save(): Promise<void> {
    return this.unsupported()
  }
  factoryReset(): Promise<void> {
    return this.unsupported()
  }
  importConfig(): Promise<void> {
    return this.unsupported()
  }
  exportConfig(): string {
    const s = this.snapshot()
    return JSON.stringify({ version: 1, learned: s.learned, params: s.params, system: s.system })
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    clearTimeout(this.auto)
    this.changes.clear()
    this.frames.clear()
    this.results.clear()
    this.stats.clear()
    void this.conn.dispose()
  }
}
