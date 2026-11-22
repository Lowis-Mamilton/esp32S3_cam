import type { DeviceInfo } from '../protocol/messages'
import type { ConnectionStatus } from '../serial/connection'
import type { TransportKind } from '../transport/types'
import type { DeviceFunctionId, FunctionId } from '../ui/layout/types'

export type { ConnectionStatus, DeviceFunctionId, DeviceInfo, FunctionId, TransportKind }

export interface Result {
  id: number // 0 = not learned
  kind: 'color' | 'tag' | 'face' | 'gesture' | 'line' | 'track' | 'qr' | 'shape' | 'motion'
  /** Box centre in 320x240 frame coordinates (not the top-left corner). */
  x: number
  y: number
  width: number
  height: number
  confidence?: number
  /**
   * AprilTag and QR code: rotation in degrees and the four corners (not clipped to the frame).
   * QR corners are the code's own top-left, top-right, bottom-right, bottom-left.
   */
  rotationDeg?: number
  corners?: [number, number][]
  /** Line tracking only (function 4). */
  line?: LineResult
  /**
   * QR code only (function 6): the content, decoded as UTF-8 (invalid bytes become U+FFFD). Undefined until
   * Studio has read it (real device: GET_RESULT_DATA, a little after the box first appears).
   */
  text?: string
  /** QR code only: the content is longer than 128 bytes and `text` is its start. */
  truncated?: boolean
}
export interface LineResult {
  /** 0 = straight up, clockwise positive. */
  angleDeg: number
  /** Bottom end x - 160. */
  offsetX: number
  start: [number, number]
  end: [number, number]
  branch: boolean
  cross: boolean
}
/** Functions that learn from a region of the picture (colour, line colour, tracked object, QR code content). */
export type LearnableFn = 1 | 4 | 5 | 6
/** Where a batch of results came from (real device only; the demo has no frame ids). */
export interface ResultsMeta {
  frameId: number
  functionId: number
}
export interface LearnedId {
  id: number
  name: string
  color?: [number, number, number] // RGB, color function only
  /** QR code (function 6) only: the content this ID stands for (GET_LEARNED_DATA), decoded like Result.text. */
  text?: string
}
export interface FunctionParams {
  color: { tolerance: number /* 0-100 */; minArea: number; maxResults: number /* 1-8 */; showNames: boolean }
  tag: { showNames: boolean }
  face: { threshold: number /* 0-100 */; maxResults: number /* 1-8 */ }
  gesture: { handThreshold: number /* 10-95 */; gestureThreshold: number /* 0-100 */; maxHands: 1 | 2 }
  line: {
    mode: 'dark' | 'light' | 'learned'
    threshold: number /* 0 = automatic, 1-255 */
    roiPercent: number /* 20-100 */
    bands: number /* 4-12 */
    minWidth: number /* 1-100 */
    maxWidth: number /* minWidth-255 */
    colorTolerance: number /* 0-100 */
  }
  track: { threshold: number /* 10-95 */; adapt: number /* 0-50 */; reacquire: boolean; multiScale: boolean }
  /** Shapes (function 11): dark shapes on light paper or the other way round. */
  shape: { mode: 'dark' | 'light'; threshold: number /* 0 = automatic, 1-255 */; minSize: number /* 16-240 */; maxResults: number /* 1-8 */ }
  /** Motion (function 12): threshold is the grey-level difference (lower = more sensitive). */
  motion: { threshold: number /* 5-100 */; minSize: number /* 8-240 */; maxResults: number /* 1-8 */ }
}
export interface SystemConfig {
  uartBaud: 115200 | 9600
  i2cAddress: number
  ledBrightness: number /* 0-100 */
}
export interface DeviceSnapshot {
  mode: 'real' | 'demo'
  status: ConnectionStatus
  info?: DeviceInfo
  /**
   * Functions the board (or the demo) can run, M7's 6, 11 and 12 included. Typed with the UI's FunctionId until the
   * UI has screens for them (then FunctionId includes 6, 11 and 12 and this type is exact).
   */
  supported: FunctionId[]
  activeFunction: DeviceFunctionId | 0
  learned: Record<DeviceFunctionId, LearnedId[]>
  params: FunctionParams
  system: SystemConfig
  dirty: boolean
  /** Real mode: the last disconnect was not requested by the user. */
  lost: boolean
  /** params/system/dirty are the device's own values (false: defaults, e.g. not connected or old firmware). */
  configKnown: boolean
  /** Preview paused: the device keeps sending RESULTS but no FRAME (STREAM_START flags 0x02). Reset on (re)connect. */
  previewPaused: boolean
  /** Real mode: how this browser can connect, USB first. Empty in demo mode and when nothing is supported. */
  transports: TransportKind[]
  /**
   * Real mode: the transport being opened or in use. After an unexpected drop (`lost`) it stays on the
   * transport that dropped; otherwise null when not connected. Always null in demo mode.
   */
  transport: TransportKind | null
}
/** Real mode and not connected: nothing can be configured yet; the hint is "connect first". */
export const needsConnection = (s: DeviceSnapshot): boolean => s.mode === 'real' && s.status !== 'connected'
/** Connected, but the firmware has no config commands (GET_CONFIG unknown): config edits need newer firmware. */
export const lacksConfig = (s: DeviceSnapshot): boolean => s.status === 'connected' && !s.configKnown

export type FrameSource =
  | { kind: 'jpeg'; jpeg: Uint8Array; frameId?: number }
  | { kind: 'canvas'; draw(ctx: CanvasRenderingContext2D): void }

export interface Device {
  snapshot(): DeviceSnapshot
  subscribe(cb: () => void): () => void // notified on any state change
  onFrame(cb: (frame: FrameSource) => void): () => void
  onResults(cb: (results: Result[], meta?: ResultsMeta) => void): () => void
  /**
   * The device switched function on another host's request (e.g. the R4). Can report 6, 11 or 12 on M7 firmware;
   * typed with the UI's FunctionId until the UI has screens for them (see `supported`).
   */
  onFunctionChange(cb: (fn: FunctionId | 0) => void): () => void
  /** Extension: stream statistics for the live view. */
  onStats(cb: (s: { fps: number; kbps: number }) => void): () => void
  /**
   * Lets the user pick a board and connects to it; call from a click handler.
   * Without `kind`: USB when the browser has it, otherwise Bluetooth.
   */
  connect(kind?: TransportKind): Promise<void>
  disconnect(): Promise<void>
  setFunction(id: DeviceFunctionId): Promise<void>
  /**
   * Learn from a picture region: colour (1), line colour (4), the object to track (5) or the content of the QR code
   * whose centre is in the region (6; the largest one wins). Resolves to the learned ID.
   */
  learn(fn: LearnableFn, region: { x: number; y: number; width: number; height: number }): Promise<number>
  forget(fn: DeviceFunctionId, id: number): Promise<void>
  forgetAll(fn: DeviceFunctionId): Promise<void>
  setName(fn: DeviceFunctionId, id: number, name: string): Promise<void>
  setParams(p: Partial<FunctionParams>): Promise<void>
  setSystem(c: Partial<SystemConfig>): Promise<void>
  /** Pause the live picture only; results keep flowing. Rejects when not connected (real mode). */
  setPreviewPaused(paused: boolean): Promise<void>
  save(): Promise<void>
  factoryReset(): Promise<void>
  exportConfig(): string
  importConfig(json: string): Promise<ImportOutcome>
  dispose(): void
}

export interface ImportOutcome {
  /**
   * Learned colours (or QR codes, or the tracked object) in the file could not be applied: a real device can only
   * learn them from its camera.
   */
  colorsNeedRelearn: boolean
}

export class NotSupportedError extends Error {
  constructor(message = 'not supported by this device') {
    super(message)
    this.name = 'NotSupportedError'
  }
}

export class NotConnectedError extends Error {
  constructor(message = 'not connected') {
    super(message)
    this.name = 'NotConnectedError'
  }
}

/** learn rejection, shared by both backends; `code` selects the message ('no-target': nothing to track in the region). */
export type LearnErrorCode = 'no-color' | 'no-target' | 'full' | 'busy' | 'cancelled'
/**
 * Every code a LearnError can carry: LearnErrorCode plus 'no-code' (no decoded QR code in the region, function 6).
 * Staging alias like DeviceFunctionId: 'no-code' moves into LearnErrorCode once the UI has a message for it.
 */
export type DeviceLearnErrorCode = LearnErrorCode | 'no-code'
export class LearnError extends Error {
  /** Can be 'no-code' (learn(6)); typed LearnErrorCode until the UI's message table has it (see DeviceLearnErrorCode). */
  readonly code: LearnErrorCode
  constructor(code: DeviceLearnErrorCode) {
    super(code)
    this.name = 'LearnError'
    this.code = code as LearnErrorCode
  }
}

/** A device refusal with a known meaning other than learning. */
export class DeviceOpError extends Error {
  readonly code: 'names-full' | 'failed'
  constructor(code: 'names-full' | 'failed', message: string) {
    super(message)
    this.name = 'DeviceOpError'
    this.code = code
  }
}

export const NAME_MAX_BYTES = 48
/** Names are UTF-8 on the wire; the limit is in bytes, not characters. */
export const nameBytes = (s: string): number => new TextEncoder().encode(s).length
/** Cuts `s` to at most `max` UTF-8 bytes without splitting a character. */
export function truncateBytes(s: string, max = NAME_MAX_BYTES): string {
  let n = 0
  let out = ''
  for (const ch of s) {
    n += nameBytes(ch)
    if (n > max) break
    out += ch
  }
  return out
}

const clampN = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, Math.round(v)))
/** Line config limits, like the clamp in the firmware (docs/protocol.md section 9). */
export function clampLineParams(l: FunctionParams['line']): FunctionParams['line'] {
  const minWidth = clampN(l.minWidth, 1, 100)
  return {
    mode: l.mode === 'light' || l.mode === 'learned' ? l.mode : 'dark',
    threshold: clampN(l.threshold, 0, 255),
    roiPercent: clampN(l.roiPercent, 20, 100),
    bands: clampN(l.bands, 4, 12),
    minWidth,
    maxWidth: Math.max(minWidth, clampN(l.maxWidth, 0, 255)),
    colorTolerance: clampN(l.colorTolerance, 0, 100),
  }
}
/** Track config limits, like the clamp in the firmware. */
export const clampTrackParams = (t: FunctionParams['track']): FunctionParams['track'] => ({
  threshold: clampN(t.threshold, 10, 95),
  adapt: clampN(t.adapt, 0, 50),
  reacquire: !!t.reacquire,
  multiScale: !!t.multiScale,
})
/** Shape config limits, like the clamp in the firmware (docs/protocol.md section 11). */
export const clampShapeParams = (s: FunctionParams['shape']): FunctionParams['shape'] => ({
  mode: s.mode === 'light' ? 'light' : 'dark',
  threshold: clampN(s.threshold, 0, 255),
  minSize: clampN(s.minSize, 16, 240),
  maxResults: clampN(s.maxResults, 1, 8),
})
/** Motion config limits, like the clamp in the firmware (docs/protocol.md section 11). */
export const clampMotionParams = (m: FunctionParams['motion']): FunctionParams['motion'] => ({
  threshold: clampN(m.threshold, 5, 100),
  minSize: clampN(m.minSize, 8, 240),
  maxResults: clampN(m.maxResults, 1, 8),
})

export const defaultParams = (): FunctionParams => ({
  color: { tolerance: 30, minArea: 100, maxResults: 4, showNames: true },
  tag: { showNames: true },
  face: { threshold: 60, maxResults: 4 },
  gesture: { handThreshold: 40, gestureThreshold: 60, maxHands: 1 },
  line: { mode: 'dark', threshold: 0, roiPercent: 60, bands: 8, minWidth: 4, maxWidth: 80, colorTolerance: 30 },
  track: { threshold: 50, adapt: 10, reacquire: true, multiScale: true },
  shape: { mode: 'dark', threshold: 0, minSize: 32, maxResults: 4 },
  motion: { threshold: 25, minSize: 24, maxResults: 4 },
})
export const defaultSystem = (): SystemConfig => ({ uartBaud: 115200, i2cAddress: 0x5a, ledBrightness: 50 })
export const emptyLearned = (): Record<DeviceFunctionId, LearnedId[]> => ({ 1: [], 2: [], 3: [], 4: [], 5: [], 6: [], 8: [], 11: [], 12: [] })

const KNOWN_FNS: readonly number[] = [1, 2, 3, 4, 5, 6, 8, 11, 12]
export const isDeviceFunctionId = (n: number): n is DeviceFunctionId => KNOWN_FNS.includes(n)
/** Function ids Studio knows, in the order given; unknown ids (newer firmware) are dropped. */
export function toFunctionIds(ids: number[]): DeviceFunctionId[] {
  return ids.filter(isDeviceFunctionId)
}

/** Tiny listener set shared by both backends. */
export class Emitter<A extends unknown[]> {
  private readonly subs = new Set<(...a: A) => void>()
  on(cb: (...a: A) => void): () => void {
    this.subs.add(cb)
    return () => this.subs.delete(cb)
  }
  emit(...a: A) {
    for (const cb of [...this.subs]) cb(...a)
  }
  clear() {
    this.subs.clear()
  }
}
