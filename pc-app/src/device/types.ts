import type { DeviceInfo } from '../protocol/messages'
import type { ConnectionStatus } from '../serial/connection'
import type { FunctionId } from '../ui/layout/types'

export type { ConnectionStatus, DeviceInfo, FunctionId }

export interface Result {
  id: number // 0 = not learned
  kind: 'color' | 'tag' | 'face'
  /** Box centre in 320x240 frame coordinates (not the top-left corner). */
  x: number
  y: number
  width: number
  height: number
  confidence?: number
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
}
/** Real mode and not connected: nothing can be configured yet; the hint is "connect first". */
export const needsConnection = (s: DeviceSnapshot): boolean => s.mode === 'real' && s.status !== 'connected'

export type FrameSource =
  | { kind: 'jpeg'; jpeg: Uint8Array }
  | { kind: 'canvas'; draw(ctx: CanvasRenderingContext2D): void }

export interface Device {
  snapshot(): DeviceSnapshot
  subscribe(cb: () => void): () => void // notified on any state change
  onFrame(cb: (frame: FrameSource) => void): () => void
  onResults(cb: (results: Result[]) => void): () => void
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
  save(): Promise<void>
  factoryReset(): Promise<void>
  exportConfig(): string
  importConfig(json: string): Promise<void>
  dispose(): void
}

export class NotSupportedError extends Error {
  constructor(message = 'not supported by this device') {
    super(message)
    this.name = 'NotSupportedError'
  }
}

export const defaultParams = (): FunctionParams => ({
  color: { tolerance: 30, minArea: 100, maxResults: 4, showNames: true },
  tag: { showNames: true },
  face: { threshold: 60, maxResults: 4 },
})
export const defaultSystem = (): SystemConfig => ({ uartBaud: 115200, i2cAddress: 0x5a, ledBrightness: 50 })
export const emptyLearned = (): Record<FunctionId, LearnedId[]> => ({ 1: [], 2: [], 3: [] })

export function toFunctionIds(ids: number[]): FunctionId[] {
  return ids.filter((i): i is FunctionId => i === 1 || i === 2 || i === 3)
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
