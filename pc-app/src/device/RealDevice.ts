import {
  Cmd,
  ErrorCode,
  encodeConfig,
  encodeForget,
  encodeFuncConfig,
  encodeGetFuncConfig,
  encodeLearn,
  encodeSetName,
  parseConfig as parseWireConfig,
  parseFuncConfig,
  parseLearned,
  FunctionId as WireFn,
  type DeviceConfig,
  type FuncConfig,
  type LineConfig,
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
  clampLineParams,
  clampTrackParams,
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
  type LearnableFn,
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
/** The firmware's name table holds 32 names over all functions (protocol.md §7 SET_NAME). */
const NAME_TABLE_SIZE = 32
const REFRESH_RETRY_MS = 500
const REFRESH_RETRIES = 3
/** Face detection has nothing learned or named, so it is never asked (GET_LEARNED is for colour and tag). */
const hasLearned = (fn: FunctionId) => fn !== 3
const KIND: Record<FunctionId, Result['kind']> = { 1: 'color', 2: 'tag', 3: 'face', 4: 'line', 5: 'track', 8: 'gesture' }
const GESTURE: FunctionId = 8
const LINE: FunctionId = 4
const TRACK: FunctionId = 5
/** Functions whose params live in GET/SET_FUNC_CONFIG (docs/protocol.md section 9). */
const FUNC_CONFIG_FNS: FunctionId[] = [LINE, TRACK, GESTURE]
/** Functions with a learned list that has colours: colour (1), line colour (4), tracked object (5, average colour). */
const hasColors = (fn: FunctionId) => fn === 1 || fn === 4 || fn === 5
const ERROR_NAME: Record<number, string> = {
  [ErrorCode.UnknownCmd]: 'UNKNOWN_CMD',
  [ErrorCode.BadLength]: 'BAD_LENGTH',
  [ErrorCode.BadParam]: 'BAD_PARAM',
  [ErrorCode.NotSupported]: 'NOT_SUPPORTED',
  [ErrorCode.Busy]: 'BUSY',
  [ErrorCode.Internal]: 'INTERNAL',
  [ErrorCode.Full]: 'FULL',
}

const isFn = (n: number): n is FunctionId => n === 1 || n === 2 || n === 3 || n === 4 || n === 5 || n === 8
const isUnsupported = (e: unknown) =>
  e instanceof MxcamError && (e.code === ErrorCode.UnknownCmd || e.code === ErrorCode.NotSupported)
const clampInt = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, Math.round(v)))
const isTimeout = (e: unknown) => e instanceof Error && !(e instanceof MxcamError) && /timeout/.test(e.message)

/** Retries once when the reply was lost (timeout); device ERRORs are not retried. */
async function retryOnTimeout<T>(op: () => Promise<T>): Promise<T> {
  try {
    return await op()
  } catch (e) {
    if (!isTimeout(e)) throw e
    return op()
  }
}

/** Per-function configs read through GET_FUNC_CONFIG; null = not read (function absent or dropped). */
interface FuncCfgs {
  gesture: FunctionParams['gesture'] | null
  line: FunctionParams['line'] | null
  track: FunctionParams['track'] | null
}
const noFuncCfgs = (): FuncCfgs => ({ gesture: null, line: null, track: null })

function toParams(c: DeviceConfig | null, fc: FuncCfgs = noFuncCfgs()): FunctionParams {
  if (!c) return { ...defaultParams(), gesture: fc.gesture ?? defaultParams().gesture, line: fc.line ?? defaultParams().line, track: fc.track ?? defaultParams().track }
  const d = defaultParams()
  return {
    gesture: fc.gesture ?? d.gesture,
    line: fc.line ?? d.line,
    track: fc.track ?? d.track,
    color: { tolerance: c.tolerance, minArea: c.minArea, maxResults: c.colorMaxResults, showNames: c.colorShowNames },
    tag: { showNames: c.tagShowNames },
    face: { threshold: c.faceThreshold, maxResults: c.faceMaxResults },
  }
}
function toSystem(c: DeviceConfig): SystemConfig {
  return { uartBaud: c.uartBaud === 9600 ? 9600 : 115200, i2cAddress: c.i2cAddress, ledBrightness: c.ledBrightness }
}
/** Same limits as the firmware's clamp (protocol.md section 10), so a value cannot wrap in its u8 field. */
const clampGesture = (g: FunctionParams['gesture']): FunctionParams['gesture'] => ({
  handThreshold: clampInt(g.handThreshold, 10, 95),
  gestureThreshold: clampInt(g.gestureThreshold, 0, 100),
  maxHands: clampInt(g.maxHands, 1, 2) as 1 | 2,
})
const sameGesture = (a: FunctionParams['gesture'], b: FunctionParams['gesture']) =>
  a.handThreshold === b.handThreshold && a.gestureThreshold === b.gestureThreshold && a.maxHands === b.maxHands
const LINE_MODES: FunctionParams['line']['mode'][] = ['dark', 'light', 'learned']
const lineToWire = (l: FunctionParams['line']): LineConfig => ({
  mode: LINE_MODES.indexOf(l.mode) as LineConfig['mode'],
  threshold: l.threshold,
  roiPct: l.roiPercent,
  bands: l.bands,
  minWidth: l.minWidth,
  maxWidth: l.maxWidth,
  colorTolerance: l.colorTolerance,
})
const lineFromWire = (w: LineConfig): FunctionParams['line'] =>
  clampLineParams({ mode: LINE_MODES[w.mode] ?? 'dark', threshold: w.threshold, roiPercent: w.roiPct, bands: w.bands, minWidth: w.minWidth, maxWidth: w.maxWidth, colorTolerance: w.colorTolerance })
const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b)

/** Full Config v1 from Studio's view; values are clamped so they cannot wrap in the u8/u16 fields. */
function toWire(base: DeviceConfig, p: Pick<FunctionParams, 'color' | 'tag' | 'face'>, s: SystemConfig): DeviceConfig {
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
  if (r.line) out.line = r.line
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
  private readonly fnChanges = new Emitter<[FunctionId | 0]>()
  private readonly now: () => number
  private status: ConnectionStatus
  private info: DeviceInfo | undefined
  private lost = false
  private userClosed = false
  /** null: not read (not connected, or firmware without GET_CONFIG). */
  private config: DeviceConfig | null = null
  /** false once the firmware answered GET_CONFIG with UNKNOWN_CMD / NOT_SUPPORTED. */
  private configSupported = true
  /** Func configs (functions 4, 5, 8; GET_FUNC_CONFIG); null until read. */
  private fc: FuncCfgs = noFuncCfgs()
  /** Functions the firmware lists but cannot configure (func config unknown): treated as not supported. */
  private readonly dropped = new Set<FunctionId>()
  private previewPaused = false
  private learned = emptyLearned()
  private active: FunctionId | 0 = 0
  /** Function changes by others are only detected once Studio has set the function itself in this session. */
  private ownFnSet = false
  private fnInFlight = 0
  private quietUntil = 0
  private snap: DeviceSnapshot | null = null
  /** Config edits run one at a time, each merged into the config the previous one left. */
  private configQueue: Promise<unknown> = Promise.resolve()
  private learnInFlight = 0
  /** Our own SET_FUNCTION switched function while a LEARN was pending: the firmware cancels it (BAD_PARAM). */
  private learnCancelled = false
  private refreshTimer: ReturnType<typeof setTimeout> | undefined
  private readonly retryFns = new Set<FunctionId>()
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
    this.fc = noFuncCfgs()
    this.dropped.clear()
    this.previewPaused = false
    this.learned = emptyLearned()
    this.active = 0
    this.ownFnSet = false
    this.fnInFlight = 0
    this.quietUntil = 0
    this.learnInFlight = 0
    this.learnCancelled = false
    clearTimeout(this.refreshTimer)
    this.refreshTimer = undefined
    this.retryFns.clear()
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
      params: toParams(c, this.fc),
      system: c ? toSystem(c) : defaultSystem(),
      dirty: c?.dirty ?? false,
      lost: this.lost,
      configKnown: c !== null,
      previewPaused: this.previewPaused,
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
  onFunctionChange(cb: (fn: FunctionId | 0) => void) {
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
    return toFunctionIds(this.info?.functionIds ?? []).filter((f) => !this.dropped.has(f))
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

  private async loadConfig(retry = false) {
    try {
      const get = () => this.call(Cmd.GetConfig)
      this.config = parseWireConfig((await (retry ? retryOnTimeout(get) : get())).payload)
      this.configSupported = true
    } catch (e) {
      if (!(e instanceof NotSupportedError)) throw e
      this.config = null
      this.configSupported = false
    }
  }

  private storeFuncConfig(c: FuncConfig) {
    if (c.functionId === WireFn.Gesture) this.fc = { ...this.fc, gesture: clampGesture(c.gesture as FunctionParams['gesture']) }
    else if (c.functionId === WireFn.Line) this.fc = { ...this.fc, line: lineFromWire(c.line) }
    else if (c.functionId === WireFn.Track) {
      const t = c.track
      this.fc = { ...this.fc, track: clampTrackParams({ threshold: t.threshold, adapt: t.adapt, reacquire: t.reacquire, multiScale: t.scale }) }
    }
  }

  /** GET_FUNC_CONFIG for function 4, 5 or 8; a firmware that cannot answer drops the function from `supported` instead of failing the connect. */
  private async loadFuncConfig(fn: FunctionId, retry = false) {
    try {
      const get = () => this.call(Cmd.GetFuncConfig, encodeGetFuncConfig(fn))
      const c = parseFuncConfig((await (retry ? retryOnTimeout(get) : get())).payload)
      if (c.functionId !== fn) throw new DeviceOpError('failed', 'GET_FUNC_CONFIG answered another function')
      this.storeFuncConfig(c)
    } catch (e) {
      // An unknown layout_version (newer firmware) is treated like "cannot answer": drop the function, keep the connection.
      if (!(e instanceof NotSupportedError) && !(e instanceof Error && /layout_version/.test(e.message))) throw e
      console.warn(`MXCam: firmware lists function ${fn} but its GET_FUNC_CONFIG is missing or unsupported; it is disabled`)
      this.dropped.add(fn)
      this.fc = { ...this.fc, ...(fn === GESTURE ? { gesture: null } : fn === LINE ? { line: null } : { track: null }) }
    }
  }
  private async loadFuncConfigs(retry = false) {
    for (const fn of FUNC_CONFIG_FNS) if (this.supportedFns().includes(fn)) await this.loadFuncConfig(fn, retry)
  }

  private async loadLearned(fn: FunctionId, retry = false) {
    let list: LearnedId[] = []
    try {
      // older firmware, or a function without learned data (BAD_PARAM): nothing learned
      const get = () =>
        this.call(Cmd.GetLearned, new Uint8Array([fn]), undefined, (code) =>
          code === ErrorCode.BadParam ? new NotSupportedError() : undefined,
        )
      const f = await (retry ? retryOnTimeout(get) : get())
      list = parseLearned(f.payload).map((i) => (hasColors(fn) ? { id: i.id, name: i.name, color: i.rgb } : { id: i.id, name: i.name }))
    } catch (e) {
      if (!(e instanceof NotSupportedError)) throw e
    }
    this.learned = { ...this.learned, [fn]: list }
  }

  /** Runs during connect (status 'connecting'): GET_CONFIG, then GET_LEARNED per supported function. A lost reply is retried once. */
  private async loadAll() {
    await this.loadConfig(true)
    await this.loadFuncConfigs(true)
    for (const fn of this.supportedFns()) if (hasLearned(fn)) await this.loadLearned(fn, true)
    this.changed()
  }

  /** After a change: re-read the learned lists and the config (for dirty). */
  private async refresh(fns: FunctionId[]) {
    for (const fn of fns) if (this.supportedFns().includes(fn) && hasLearned(fn)) await this.loadLearned(fn)
    if (this.configSupported) await this.loadConfig()
    await this.loadFuncConfigs()
    this.changed()
  }

  /**
   * Refresh after an operation the device already accepted: the operation succeeded, so a failed re-read is not
   * reported as a failure; it is retried in the background instead.
   */
  private async refreshAfter(fns: FunctionId[], attempt = 0): Promise<void> {
    try {
      await this.refresh(fns)
    } catch {
      if (attempt + 1 >= REFRESH_RETRIES || this.status !== 'connected' || this.disposed) return
      // One pending retry covers every function still waiting for a re-read.
      for (const fn of fns) this.retryFns.add(fn)
      clearTimeout(this.refreshTimer)
      this.refreshTimer = setTimeout(() => {
        const all = [...this.retryFns]
        this.retryFns.clear()
        void this.refreshAfter(all, attempt + 1)
      }, REFRESH_RETRY_MS)
    }
  }

  private onResultsPush(r: ResultsPush) {
    const fn = r.functionId
    if (
      (fn === 0 || (isFn(fn) && this.supportedFns().includes(fn))) &&
      fn !== this.active &&
      this.ownFnSet &&
      this.fnInFlight === 0 &&
      this.now() >= this.quietUntil
    ) {
      // Someone else (e.g. the R4 over UART/I2C) changed the function (0 = none).
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
    if (this.learnInFlight > 0 && id !== this.active) this.learnCancelled = true
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

  async setPreviewPaused(paused: boolean): Promise<void> {
    this.ensureConnected()
    if (paused === this.previewPaused) return
    await this.conn.setFrames(!paused) // STREAM_START with flags 0x02 (results only) / 0x03; max_fps and quality are kept
    this.previewPaused = paused
    this.changed()
  }

  async learn(fn: LearnableFn, region: { x: number; y: number; width: number; height: number }): Promise<number> {
    this.ensureConnected()
    this.requireFn(fn)
    const x = clampInt(region.x, 0, FRAME_W - 1)
    const y = clampInt(region.y, 0, FRAME_H - 1)
    const rect = { x, y, width: clampInt(region.width, 1, FRAME_W - x), height: clampInt(region.height, 1, FRAME_H - y) }
    this.learnInFlight++
    let f: Frame
    try {
      f = await this.call(Cmd.Learn, encodeLearn(fn, rect), LEARN_TIMEOUT_MS, (code) => {
        if (code === ErrorCode.Full) return new LearnError('full')
        if (code === ErrorCode.Busy) return new LearnError('busy')
        // BAD_PARAM: no colour (1, 4) or no usable object (5) in the region, or the LEARN was cancelled by our function switch
        if (code === ErrorCode.BadParam) return new LearnError(this.learnCancelled ? 'cancelled' : fn === TRACK ? 'no-target' : 'no-color')
        return undefined
      })
    } finally {
      if (--this.learnInFlight <= 0) {
        this.learnInFlight = 0
        this.learnCancelled = false
      }
    }
    await this.refreshAfter([fn])
    if (f.payload.length !== 1) throw new DeviceOpError('failed', `0x30 reply length ${f.payload.length}`)
    return f.payload[0]
  }

  async forget(fn: FunctionId, id: number): Promise<void> {
    this.ensureConnected()
    this.requireFn(fn)
    await this.call(Cmd.Forget, encodeForget(fn, id))
    await this.refreshAfter([fn])
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
    await this.refreshAfter([fn])
  }

  /** SET_CONFIG with the whole config; the device answers with what it applied (clamped). */
  private async applyConfig(next: DeviceConfig) {
    const cur = this.requireConfig()
    if (sameConfig(cur, next)) return
    this.config = parseWireConfig((await this.call(Cmd.SetConfig, encodeConfig(next))).payload)
    this.changed()
  }

  /** SET_FUNC_CONFIG: the device answers with what it applied (clamped). A real change marks the config dirty, like the firmware. */
  private async sendFuncConfig(c: FuncConfig): Promise<FuncConfig> {
    const reply = parseFuncConfig((await this.call(Cmd.SetFuncConfig, encodeFuncConfig(c))).payload)
    if (reply.functionId !== c.functionId) throw new DeviceOpError('failed', 'SET_FUNC_CONFIG answered another function')
    return reply
  }
  private markDirtyIf(changed: boolean) {
    if (this.config && changed) this.config = { ...this.config, dirty: true }
  }

  private async applyGesture(want: FunctionParams['gesture']) {
    const next = clampGesture(want)
    const old = this.fc.gesture ?? defaultParams().gesture
    if (this.fc.gesture && sameGesture(old, next)) return
    this.storeFuncConfig(await this.sendFuncConfig({ functionId: WireFn.Gesture, gesture: next }))
    this.markDirtyIf(!sameGesture(old, this.fc.gesture ?? old))
    this.changed()
  }
  private async applyLine(want: FunctionParams['line']) {
    const next = clampLineParams(want)
    const old = this.fc.line ?? defaultParams().line
    if (this.fc.line && same(old, next)) return
    this.storeFuncConfig(await this.sendFuncConfig({ functionId: WireFn.Line, line: lineToWire(next) }))
    this.markDirtyIf(!same(old, this.fc.line ?? old))
    this.changed()
  }
  private async applyTrack(want: FunctionParams['track']) {
    const next = clampTrackParams(want)
    const old = this.fc.track ?? defaultParams().track
    if (this.fc.track && same(old, next)) return
    this.storeFuncConfig(
      await this.sendFuncConfig({
        functionId: WireFn.Track,
        track: { threshold: next.threshold, adapt: next.adapt, reacquire: next.reacquire, scale: next.multiScale },
      }),
    )
    this.markDirtyIf(!same(old, this.fc.track ?? old))
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
      if (p.gesture) this.requireFn(GESTURE)
      if (p.line) this.requireFn(LINE)
      if (p.track) this.requireFn(TRACK)
      const old = toParams(cur, this.fc)
      const params = { color: { ...old.color, ...p.color }, tag: { ...old.tag, ...p.tag }, face: { ...old.face, ...p.face } }
      await this.applyConfig(toWire(cur, params, toSystem(cur)))
      if (p.gesture) await this.applyGesture({ ...old.gesture, ...p.gesture })
      if (p.line) await this.applyLine({ ...old.line, ...p.line })
      if (p.track) await this.applyTrack({ ...old.track, ...p.track })
    })
  }

  setSystem(c: Partial<SystemConfig>): Promise<void> {
    return this.queued(async () => {
      this.ensureConnected()
      const cur = this.requireConfig()
      await this.applyConfig(toWire(cur, toParams(cur, this.fc), { ...toSystem(cur), ...c }))
    })
  }

  save(): Promise<void> {
    return this.queued(async () => {
      this.ensureConnected()
      await this.call(Cmd.Save, undefined, FLASH_TIMEOUT_MS)
      await this.refreshAfter([])
    })
  }

  factoryReset(): Promise<void> {
    return this.queued(async () => {
      this.ensureConnected()
      await this.call(Cmd.FactoryReset, undefined, FLASH_TIMEOUT_MS)
      await this.refreshAfter(this.supportedFns())
    })
  }

  exportConfig(): string {
    this.ensureConnected()
    const c = this.requireConfig()
    const { gesture, line, track, ...params } = toParams(c, this.fc)
    // No func config read from the board (old firmware): omit it rather than export made-up defaults.
    const out = { ...params, ...(this.fc.gesture ? { gesture } : {}), ...(this.fc.line ? { line } : {}), ...(this.fc.track ? { track } : {}) }
    return JSON.stringify({ version: 1, learned: this.learned, params: out, system: toSystem(c) })
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
    const fns = this.supportedFns()
    // Check the shared name table first, so a file that cannot fit never wipes the tag names.
    if (this.namesAfterImport(file, fns) > NAME_TABLE_SIZE) throw new DeviceOpError('names-full', 'name table full')
    await this.applyConfig(toWire(cur, file.params, file.system))
    if (fns.includes(GESTURE) && file.params.gesture) await this.applyGesture(file.params.gesture)
    if (fns.includes(LINE) && file.params.line) await this.applyLine(file.params.line)
    if (fns.includes(TRACK) && file.params.track) await this.applyTrack(file.params.track)
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
    // The line colour is sampled from the camera too: a file colour the board does not hold has to be learned again.
    if (fns.includes(LINE)) {
      const e = file.learned[4][0]
      const board = this.learned[4][0]
      if (e && !(board?.color && e.color && board.color.every((v, i) => v === e.color![i]))) colorsNeedRelearn = true
    }
    // The tracked object is a template from the camera: it cannot come from a file. Only its name is applied (when the board has one).
    if (fns.includes(TRACK)) {
      const e = file.learned[5][0]
      const board = this.learned[5][0]
      if (e && !board) colorsNeedRelearn = true
      else if (e && board && board.name !== e.name) await this.sendName(TRACK, 1, e.name)
    }
    if (fns.includes(2)) {
      await this.call(Cmd.Forget, encodeForget(2, FORGET_ALL))
      for (const e of file.learned[2]) if (e.name) await this.sendName(2, e.id, e.name)
    }
    // Old files have no gesture names, so an empty list leaves the board's names alone.
    if (fns.includes(GESTURE) && file.learned[8].length > 0) {
      await this.call(Cmd.Forget, encodeForget(GESTURE, FORGET_ALL))
      for (const e of file.learned[8]) if (e.name) await this.sendName(GESTURE, e.id, e.name)
    }
    await this.refreshAfter(fns)
    return { colorsNeedRelearn }
  }

  /** Names on the board after applying `file`: colour names stay per board ID, tag names come from the file. */
  private namesAfterImport(file: ReturnType<typeof parseConfig>, fns: FunctionId[]): number {
    const colours = this.learned[1].filter((b) => {
      const e = file.learned[1].find((x) => x.id === b.id)
      const want = e?.color
      const same = !!want && !!b.color && b.color.every((v, i) => v === want[i])
      return (same && e ? e.name : b.name) !== ''
    }).length
    const tags = (fns.includes(2) ? file.learned[2] : this.learned[2]).filter((e) => e.name !== '').length
    const gestures = (fns.includes(GESTURE) ? (file.learned[8].length > 0 ? file.learned[8] : this.learned[8]) : []).filter((e) => e.name !== '').length
    const e5 = file.learned[5][0]
    const b5 = this.learned[5][0]
    const track = fns.includes(TRACK) && b5 ? ((e5 ? e5.name : b5.name) !== '' ? 1 : 0) : 0
    return colours + tags + gestures + track
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    clearTimeout(this.auto)
    clearTimeout(this.refreshTimer)
    this.changes.clear()
    this.frames.clear()
    this.results.clear()
    this.stats.clear()
    this.fnChanges.clear()
    void this.conn.dispose()
  }
}
