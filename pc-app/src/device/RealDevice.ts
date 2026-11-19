import {
  Cmd,
  ErrorCode,
  encodeConfig,
  encodeForget,
  encodeLearn,
  encodeSetName,
  parseConfig as parseWireConfig,
  parseLearned,
  type DeviceConfig,
  type ResultRecord,
  type ResultsPush,
} from '../protocol/messages'
import type { Frame } from '../protocol/codec'
import { MxcamConnection, MxcamError, type ConnectionStatus } from '../serial/connection'
import { parseConfig } from './config'
import {
  DeviceOpError,
  Emitter,
  LearnError,
  NAME_MAX_BYTES,
  NotConnectedError,
  NotSupportedError,
  defaultParams,
  defaultSystem,
  emptyLearned,
  nameBytes,
  toFunctionIds,
  type Device,
  type DeviceInfo,
  type DeviceSnapshot,
  type FrameSource,
  type FunctionId,
  type FunctionParams,
  type ImportOutcome,
  type LearnedId,
  type Result,
  type ResultsMeta,
  type SystemConfig,
} from './types'

const FRAME_W = 320
const FRAME_H = 240
/** LEARN is answered after the next processed frame (protocol.md §7). */
const LEARN_TIMEOUT_MS = 2500
/** SAVE / FACTORY_RESET are answered after the flash write. */
const FLASH_TIMEOUT_MS = 5000
/** RESULTS of another function within this window after our own SET_FUNCTION are stale in-flight frames. */
const OWN_SWITCH_QUIET_MS = 1000
const FORGET_ALL = 0xffff
const KIND: Record<FunctionId, Result['kind']> = { 1: 'color', 2: 'tag', 3: 'face' }
const ERROR_NAME: Record<number, string> = {
  [ErrorCode.UnknownCmd]: 'UNKNOWN_CMD',
  [ErrorCode.BadLength]: 'BAD_LENGTH',
  [ErrorCode.BadParam]: 'BAD_PARAM',
  [ErrorCode.NotSupported]: 'NOT_SUPPORTED',
  [ErrorCode.Busy]: 'BUSY',
  [ErrorCode.Internal]: 'INTERNAL',
  [ErrorCode.Full]: 'FULL',
}

const isFn = (n: number): n is FunctionId => n === 1 || n === 2 || n === 3
const isUnsupported = (e: unknown) =>
  e instanceof MxcamError && (e.code === ErrorCode.UnknownCmd || e.code === ErrorCode.NotSupported)
const clampInt = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, Math.round(v)))

function toParams(c: DeviceConfig): FunctionParams {
  return {
    color: { tolerance: c.tolerance, minArea: c.minArea, maxResults: c.colorMaxResults, showNames: c.colorShowNames },
    tag: { showNames: c.tagShowNames },
    face: { threshold: c.faceThreshold, maxResults: c.faceMaxResults },
  }
}
function toSystem(c: DeviceConfig): SystemConfig {
  return { uartBaud: c.uartBaud === 9600 ? 9600 : 115200, i2cAddress: c.i2cAddress, ledBrightness: c.ledBrightness }
}
/** Full Config v1 from Studio's view; values are clamped so they cannot wrap in the u8/u16 fields. */
function toWire(base: DeviceConfig, p: FunctionParams, s: SystemConfig): DeviceConfig {
  return {
    tolerance: clampInt(p.color.tolerance, 0, 100),
    minArea: clampInt(p.color.minArea, 0, 0xffff),
    colorMaxResults: clampInt(p.color.maxResults, 1, 8),
    colorShowNames: p.color.showNames,
    tagShowNames: p.tag.showNames,
    faceThreshold: clampInt(p.face.threshold, 0, 100),
    faceMaxResults: clampInt(p.face.maxResults, 1, 8),
    uartBaud: s.uartBaud,
    i2cAddress: clampInt(s.i2cAddress, 0, 0xff),
    ledBrightness: clampInt(s.ledBrightness, 0, 100),
    dirty: base.dirty,
  }
}
const sameConfig = (a: DeviceConfig, b: DeviceConfig) =>
  JSON.stringify({ ...a, dirty: false }) === JSON.stringify({ ...b, dirty: false })

function toResult(r: ResultRecord, kind: Result['kind']): Result {
  const out: Result = { id: r.id, kind, x: r.x, y: r.y, width: r.width, height: r.height }
  if (r.confidence !== undefined) out.confidence = r.confidence
  if (r.extra) {
    out.rotationDeg = r.extra.rotationDeg
    out.corners = r.extra.corners
  }
  return out
}

type ErrorMap = (code: number) => Error | undefined

/** Drives an M2 MXCam over MXCam Protocol v1 (docs/protocol.md §6-§7). The snapshot is read from the device. */
export class RealDevice implements Device {
  private readonly conn: MxcamConnection
  private readonly changes = new Emitter<[]>()
  private readonly frames = new Emitter<[FrameSource]>()
  private readonly results = new Emitter<[Result[], ResultsMeta | undefined]>()
  private readonly stats = new Emitter<[{ fps: number; kbps: number }]>()
  private readonly fnChanges = new Emitter<[FunctionId]>()
  private readonly now: () => number
  private status: ConnectionStatus
  private info: DeviceInfo | undefined
  private lost = false
  private userClosed = false
  /** null: not read (not connected, or firmware without GET_CONFIG). */
  private config: DeviceConfig | null = null
  /** false once the firmware answered GET_CONFIG with UNKNOWN_CMD / NOT_SUPPORTED. */
  private configSupported = true
  private learned = emptyLearned()
  private active: FunctionId | 0 = 0
  /** Function changes by others are only detected once Studio has set the function itself in this session. */
  private ownFnSet = false
  private fnInFlight = 0
  private quietUntil = 0
  private snap: DeviceSnapshot | null = null
  /** Config edits run one at a time, each merged into the config the previous one left. */
  private configQueue: Promise<unknown> = Promise.resolve()
  private readonly auto: ReturnType<typeof setTimeout>
  private disposed = false

  constructor(serial: Serial | undefined, now: () => number = () => performance.now()) {
    this.now = now
    this.status = serial ? 'disconnected' : 'unsupported'
    this.conn = new MxcamConnection(
      serial,
      {
        status: (s) => {
          const wasConnected = this.status === 'connected'
          this.status = s
          if (s === 'connecting') {
            this.userClosed = false
            this.lost = false
            this.reset()
          } else if (s === 'disconnected') {
            // lost only when an established connection dropped; a failed open/auto-connect is not a loss
            this.lost = wasConnected && !this.userClosed
            this.info = undefined
            this.reset()
          }
          this.changed()
        },
        frame: (f) => this.frames.emit({ kind: 'jpeg', jpeg: f.jpeg, frameId: f.frameId }),
        stats: (s) => this.stats.emit(s),
        info: (i) => {
          this.info = i
          this.changed()
        },
        setup: () => this.loadAll(),
        results: (r) => this.onResultsPush(r),
      },
      { streamResults: true },
    )
    // Deferred so StrictMode's mount/unmount/mount does not race two opens of one port.
    this.auto = setTimeout(() => void this.conn.autoConnect(), 0)
  }

  private reset() {
    this.config = null
    this.configSupported = true
    this.learned = emptyLearned()
    this.active = 0
    this.ownFnSet = false
    this.fnInFlight = 0
    this.quietUntil = 0
  }

  private changed() {
    this.snap = null
    this.changes.emit()
  }

  snapshot(): DeviceSnapshot {
    const c = this.config
    return (this.snap ??= {
      mode: 'real',
      status: this.status,
      info: this.info,
      supported: this.supportedFns(),
      activeFunction: this.active,
      learned: this.learned,
      params: c ? toParams(c) : defaultParams(),
      system: c ? toSystem(c) : defaultSystem(),
      dirty: c?.dirty ?? false,
      lost: this.lost,
      configKnown: c !== null,
    })
  }

  subscribe(cb: () => void) {
    return this.changes.on(cb)
  }
  onFrame(cb: (frame: FrameSource) => void) {
    return this.frames.on(cb)
  }
  onResults(cb: (results: Result[], meta?: ResultsMeta) => void) {
    return this.results.on(cb)
  }
  onFunctionChange(cb: (fn: FunctionId) => void) {
    return this.fnChanges.on(cb)
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

  // ---- protocol helpers ----

  private supportedFns(): FunctionId[] {
    return toFunctionIds(this.info?.functionIds ?? [])
  }
  private ensureConnected() {
    if (this.status !== 'connected') throw new NotConnectedError()
  }
  private requireFn(fn: FunctionId) {
    if (!this.supportedFns().includes(fn)) throw new NotSupportedError()
  }
  private requireConfig(): DeviceConfig {
    if (!this.config) throw this.configSupported ? new NotConnectedError() : new NotSupportedError()
    return this.config
  }

  /** One request; device ERRORs become NotSupportedError, the op-specific error from `map`, or a readable error. */
  private async call(cmd: number, payload: Uint8Array = new Uint8Array(0), timeoutMs?: number, map?: ErrorMap): Promise<Frame> {
    try {
      return await this.conn.request(cmd, payload, timeoutMs)
    } catch (e) {
      if (!(e instanceof MxcamError)) throw e
      if (isUnsupported(e)) throw new NotSupportedError()
      const mapped = map?.(e.code)
      if (mapped) throw mapped
      const name = ERROR_NAME[e.code] ?? `code ${e.code}`
      throw new DeviceOpError('failed', `0x${cmd.toString(16).padStart(2, '0')} ${name}`)
    }
  }

  private async loadConfig() {
    try {
      this.config = parseWireConfig((await this.call(Cmd.GetConfig)).payload)
      this.configSupported = true
    } catch (e) {
      if (!(e instanceof NotSupportedError)) throw e
      this.config = null
      this.configSupported = false
    }
  }

  private async loadLearned(fn: FunctionId) {
    let list: LearnedId[] = []
    try {
      // older firmware, or a function without learned data (BAD_PARAM): nothing learned
      const f = await this.call(Cmd.GetLearned, new Uint8Array([fn]), undefined, (code) =>
        code === ErrorCode.BadParam ? new NotSupportedError() : undefined,
      )
      list = parseLearned(f.payload).map((i) => (fn === 1 ? { id: i.id, name: i.name, color: i.rgb } : { id: i.id, name: i.name }))
    } catch (e) {
      if (!(e instanceof NotSupportedError)) throw e
    }
    this.learned = { ...this.learned, [fn]: list }
  }

  /** Runs during connect (status 'connecting'): GET_CONFIG, then GET_LEARNED per supported function. */
  private async loadAll() {
    await this.loadConfig()
    for (const fn of this.supportedFns()) await this.loadLearned(fn)
    this.changed()
  }

  /** After a change: re-read the learned lists and the config (for dirty). */
  private async refresh(fns: FunctionId[]) {
    for (const fn of fns) if (this.supportedFns().includes(fn)) await this.loadLearned(fn)
    if (this.configSupported) await this.loadConfig()
    this.changed()
  }

  private onResultsPush(r: ResultsPush) {
    const fn = r.functionId
    if (
      isFn(fn) &&
      fn !== this.active &&
      this.ownFnSet &&
      this.fnInFlight === 0 &&
      this.now() >= this.quietUntil &&
      this.supportedFns().includes(fn)
    ) {
      // Someone else (e.g. the R4 over UART/I2C) changed the function.
      this.active = fn
      this.changed()
      this.fnChanges.emit(fn)
    }
    const results = isFn(fn) ? r.results.map((rec) => toResult(rec, KIND[fn])) : []
    this.results.emit(results, { frameId: r.frameId, functionId: fn })
  }

  // ---- operations ----

  async setFunction(id: FunctionId): Promise<void> {
    this.ensureConnected()
    this.requireFn(id)
    this.fnInFlight++
    this.quietUntil = this.now() + OWN_SWITCH_QUIET_MS
    try {
      const f = await this.call(Cmd.SetFunction, new Uint8Array([id]))
      const got = f.payload[0] ?? id
      this.active = isFn(got) ? got : 0
      this.ownFnSet = true
    } finally {
      this.fnInFlight = Math.max(0, this.fnInFlight - 1)
      this.quietUntil = Math.max(this.quietUntil, this.now() + OWN_SWITCH_QUIET_MS)
    }
    this.changed()
  }

  async learnColor(region: { x: number; y: number; width: number; height: number }): Promise<number> {
    this.ensureConnected()
    this.requireFn(1)
    const x = clampInt(region.x, 0, FRAME_W - 1)
    const y = clampInt(region.y, 0, FRAME_H - 1)
    const rect = { x, y, width: clampInt(region.width, 1, FRAME_W - x), height: clampInt(region.height, 1, FRAME_H - y) }
    const f = await this.call(Cmd.Learn, encodeLearn(1, rect), LEARN_TIMEOUT_MS, (code) =>
      code === ErrorCode.Full ? new LearnError('full') : code === ErrorCode.BadParam ? new LearnError('no-color') : undefined,
    )
    await this.refresh([1])
    return f.payload[0]
  }

  async forget(fn: FunctionId, id: number): Promise<void> {
    this.ensureConnected()
    this.requireFn(fn)
    await this.call(Cmd.Forget, encodeForget(fn, id))
    await this.refresh([fn])
  }

  forgetAll(fn: FunctionId): Promise<void> {
    return this.forget(fn, FORGET_ALL)
  }

  private sendName(fn: FunctionId, id: number, name: string) {
    return this.call(Cmd.SetName, encodeSetName(fn, id, name), undefined, (code) =>
      code === ErrorCode.Full ? new DeviceOpError('names-full', 'name table full') : undefined,
    )
  }

  async setName(fn: FunctionId, id: number, name: string): Promise<void> {
    this.ensureConnected()
    this.requireFn(fn)
    const n = nameBytes(name)
    if (n > NAME_MAX_BYTES) throw new Error(`name is ${n} bytes in UTF-8, max ${NAME_MAX_BYTES}`)
    await this.sendName(fn, id, name)
    await this.refresh([fn])
  }

  /** SET_CONFIG with the whole config; the device answers with what it applied (clamped). */
  private async applyConfig(next: DeviceConfig) {
    const cur = this.requireConfig()
    if (sameConfig(cur, next)) return
    this.config = parseWireConfig((await this.call(Cmd.SetConfig, encodeConfig(next))).payload)
    this.changed()
  }

  private queued<T>(op: () => Promise<T>): Promise<T> {
    const run = this.configQueue.then(op, op)
    this.configQueue = run.catch(() => {})
    return run
  }

  setParams(p: Partial<FunctionParams>): Promise<void> {
    return this.queued(async () => {
      this.ensureConnected()
      const cur = this.requireConfig()
      const old = toParams(cur)
      const params = { color: { ...old.color, ...p.color }, tag: { ...old.tag, ...p.tag }, face: { ...old.face, ...p.face } }
      await this.applyConfig(toWire(cur, params, toSystem(cur)))
    })
  }

  setSystem(c: Partial<SystemConfig>): Promise<void> {
    return this.queued(async () => {
      this.ensureConnected()
      const cur = this.requireConfig()
      await this.applyConfig(toWire(cur, toParams(cur), { ...toSystem(cur), ...c }))
    })
  }

  async save(): Promise<void> {
    this.ensureConnected()
    await this.call(Cmd.Save, undefined, FLASH_TIMEOUT_MS)
    await this.refresh([])
  }

  async factoryReset(): Promise<void> {
    this.ensureConnected()
    await this.call(Cmd.FactoryReset, undefined, FLASH_TIMEOUT_MS)
    await this.loadAll()
  }

  exportConfig(): string {
    this.ensureConnected()
    const c = this.requireConfig()
    return JSON.stringify({ version: 1, learned: this.learned, params: toParams(c), system: toSystem(c) })
  }

  /**
   * Applies the file's params, system and names. Learned colours cannot be re-created from a file (the device
   * samples them from its camera), so colour names are only applied to IDs that hold the same colour on the board.
   */
  async importConfig(json: string): Promise<ImportOutcome> {
    const file = parseConfig(json) // throws before anything is sent
    return this.queued(() => this.applyFile(file))
  }

  private async applyFile(file: ReturnType<typeof parseConfig>): Promise<ImportOutcome> {
    this.ensureConnected()
    const cur = this.requireConfig()
    await this.applyConfig(toWire(cur, file.params, file.system))
    const fns = this.supportedFns()
    let colorsNeedRelearn = false
    if (fns.includes(1)) {
      for (const e of file.learned[1]) {
        const board = this.learned[1].find((l) => l.id === e.id)
        const want = e.color
        const same = !!board?.color && !!want && board.color.every((v, i) => v === want[i])
        if (!board || !same) {
          colorsNeedRelearn = true
          continue
        }
        if (board.name !== e.name) await this.sendName(1, e.id, e.name)
      }
    }
    if (fns.includes(2)) {
      await this.call(Cmd.Forget, encodeForget(2, FORGET_ALL))
      for (const e of file.learned[2]) if (e.name) await this.sendName(2, e.id, e.name)
    }
    await this.refresh(fns)
    return { colorsNeedRelearn }
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    clearTimeout(this.auto)
    this.changes.clear()
    this.frames.clear()
    this.results.clear()
    this.stats.clear()
    this.fnChanges.clear()
    void this.conn.dispose()
  }
}
