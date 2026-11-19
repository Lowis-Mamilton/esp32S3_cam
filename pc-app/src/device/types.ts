import type { DeviceInfo } from '../protocol/messages'
import type { ConnectionStatus } from '../serial/connection'
import type { FunctionId } from '../ui/layout/types'

export type { ConnectionStatus, DeviceInfo, FunctionId }

export interface Result {
  id: number // 0 = not learned
  kind: 'color' | 'tag' | 'face' | 'gesture'
  /** Box centre in 320x240 frame coordinates (not the top-left corner). */
  x: number
  y: number
  width: number
  height: number
  confidence?: number
  /** AprilTag only: rotation in degrees and the four corners (not clipped to the frame). */
  rotationDeg?: number
  corners?: [number, number][]
}
/** Where a batch of results came from (real device only; the demo has no frame ids). */
export interface ResultsMeta {
  frameId: number
  functionId: number
}
export interface LearnedId {
  id: number
  name: string
  color?: [number, number, number] // RGB, color function only
}
export interface FunctionParams {
  color: { tolerance: number /* 0-100 */; minArea: number; maxResults: number /* 1-8 */; showNames: boolean }
  tag: { showNames: boolean }
  face: { threshold: number /* 0-100 */; maxResults: number /* 1-8 */ }
  gesture: { handThreshold: number /* 10-95 */; gestureThreshold: number /* 0-100 */; maxHands: 1 | 2 }
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
  supported: FunctionId[]
  activeFunction: FunctionId | 0
  learned: Record<FunctionId, LearnedId[]>
  params: FunctionParams
  system: SystemConfig
  dirty: boolean
  /** Real mode: the last disconnect was not requested by the user. */
  lost: boolean
  /** params/system/dirty are the device's own values (false: defaults, e.g. not connected or old firmware). */
  configKnown: boolean
  /** Preview paused: the device keeps sending RESULTS but no FRAME (STREAM_START flags 0x02). Reset on (re)connect. */
  previewPaused: boolean
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
  /** The device switched function on another host's request (e.g. the R4). */
  onFunctionChange(cb: (fn: FunctionId | 0) => void): () => void
  /** Extension: stream statistics for the live view. */
  onStats(cb: (s: { fps: number; kbps: number }) => void): () => void
  connect(): Promise<void>
  disconnect(): Promise<void>
  setFunction(id: FunctionId): Promise<void>
  learnColor(region: { x: number; y: number; width: number; height: number }): Promise<number>
  forget(fn: FunctionId, id: number): Promise<void>
  forgetAll(fn: FunctionId): Promise<void>
  setName(fn: FunctionId, id: number, name: string): Promise<void>
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
  /** Learned colours in the file could not be applied (a real device can only learn from its camera). */
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

/** learnColor rejection, shared by both backends; `code` selects the message. */
export type LearnErrorCode = 'no-color' | 'full' | 'busy' | 'cancelled'
export class LearnError extends Error {
  readonly code: LearnErrorCode
  constructor(code: LearnErrorCode) {
    super(code)
    this.name = 'LearnError'
    this.code = code
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

export const defaultParams = (): FunctionParams => ({
  color: { tolerance: 30, minArea: 100, maxResults: 4, showNames: true },
  tag: { showNames: true },
  face: { threshold: 60, maxResults: 4 },
  gesture: { handThreshold: 40, gestureThreshold: 60, maxHands: 1 },
})
export const defaultSystem = (): SystemConfig => ({ uartBaud: 115200, i2cAddress: 0x5a, ledBrightness: 50 })
export const emptyLearned = (): Record<FunctionId, LearnedId[]> => ({ 1: [], 2: [], 3: [], 8: [] })

export function toFunctionIds(ids: number[]): FunctionId[] {
  return ids.filter((i): i is FunctionId => i === 1 || i === 2 || i === 3 || i === 8)
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
