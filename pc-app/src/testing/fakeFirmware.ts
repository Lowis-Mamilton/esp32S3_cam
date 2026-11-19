// Test helper: a fake Web Serial port that behaves like the M2 firmware (docs/protocol.md §3, §6, §7).
import { Decoder, encodeFrame, type Frame } from '../protocol/codec'
import { Cmd, ErrorCode, encodeConfig, parseConfig, responseOf, type DeviceConfig } from '../protocol/messages'

/** Gesture config v1 values (docs/protocol.md section 10). */
export interface FakeGestureConfig {
  handThreshold: number
  gestureThreshold: number
  maxHands: number
}
export const DEFAULT_GESTURE: FakeGestureConfig = { handThreshold: 40, gestureThreshold: 60, maxHands: 1 }
const GESTURE_FN = 8
const clampN = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v))

export const DEFAULT_CONFIG: DeviceConfig = {
  tolerance: 30,
  minArea: 100,
  colorMaxResults: 4,
  colorShowNames: true,
  tagShowNames: true,
  faceThreshold: 60,
  faceMaxResults: 4,
  uartBaud: 115200,
  i2cAddress: 0x5a,
  ledBrightness: 50,
  dirty: false,
}

export interface FakeRecord {
  id: number
  x: number
  y: number
  width: number
  height: number
  confidence?: number
  tag?: { rotationDeg: number; corners: [number, number][] }
}

export function encodeResults(frameId: number, functionId: number, records: FakeRecord[]): Uint8Array {
  const size = 6 + records.reduce((n, r) => n + 12 + (r.tag ? 18 : 0), 0)
  const out = new Uint8Array(size)
  const v = new DataView(out.buffer)
  v.setUint32(0, frameId, true)
  out[4] = functionId
  out[5] = records.length
  let o = 6
  for (const r of records) {
    v.setUint16(o, r.id, true)
    v.setInt16(o + 2, r.x, true)
    v.setInt16(o + 4, r.y, true)
    v.setUint16(o + 6, r.width, true)
    v.setUint16(o + 8, r.height, true)
    out[o + 10] = r.confidence ?? 255
    out[o + 11] = r.tag ? 18 : 0
    o += 12
    if (r.tag) {
      v.setInt16(o, Math.round(r.tag.rotationDeg * 100), true)
      r.tag.corners.forEach(([x, y], k) => {
        v.setInt16(o + 2 + 4 * k, x, true)
        v.setInt16(o + 4 + 4 * k, y, true)
      })
      o += 18
    }
  }
  return out
}

function infoPayload(functionIds: number[]): Uint8Array {
  const p = new Uint8Array(23 + functionIds.length)
  p[0] = 1
  p.set([0, 2, 0], 1)
  p.set(new TextEncoder().encode('784715a'), 4)
  p.set([1, 2, 3, 4, 5, 6], 11)
  new DataView(p.buffer).setUint16(17, 320, true)
  new DataView(p.buffer).setUint16(19, 240, true)
  p[21] = 8
  p[22] = functionIds.length
  p.set(functionIds, 23)
  return p
}

type Reply = Uint8Array | { error: number } | null

/** Simulated M2 firmware. Mutate the public fields to set up a scenario. */
export class FakeFirmware {
  /** Capabilities reported by GET_INFO; add 3 for a firmware with face detection (M3). */
  functionIds = [1, 2]
  active = 0
  config: DeviceConfig = { ...DEFAULT_CONFIG }
  /** Gesture config v1 (function 8; GET/SET_FUNC_CONFIG). Add 8 to functionIds to enable it. */
  gesture: FakeGestureConfig = { ...DEFAULT_GESTURE }
  /** flags byte of the last STREAM_START (bit0 FRAME, bit1 RESULTS); pushFrame sends nothing when bit0 is 0, like the firmware. */
  streamFlags = 0
  colors: { id: number; rgb: [number, number, number] }[] = []
  /** key `${fn}:${id}` */
  names = new Map<string, string>()
  /** Colour sampled by the next LEARN; null = region has no colour. */
  learnRgb: [number, number, number] | null = [230, 40, 36]
  /** LEARN answers after this delay (the firmware samples the next frame). */
  learnDelayMs = 100
  saveDelayMs = 0
  /** Commands answered with UNKNOWN_CMD (simulates older firmware). */
  unknown = new Set<number>()
  /** The next n SET_FUNCTION requests answer INTERNAL (and change nothing). */
  failSetFunction = 0
  /** Per-command error override: cmd -> error code. */
  failWith = new Map<number, number>()
  /** cmd -> how many of the next requests to leave unanswered (lost reply). */
  drop = new Map<number, number>()
  /** cmd -> reply delay in ms, so a later request can be answered first (out of order). */
  replyDelay = new Map<number, number>()
  /** Replaces the LEARN reply payload (e.g. a malformed one). */
  learnReply: Uint8Array | null = null
  private pendingLearn: { f: Frame; timer: ReturnType<typeof setTimeout> } | null = null
  requests: Frame[] = []
  readonly port: {
    getInfo(): SerialPortInfo
    setSignals(): Promise<void>
    open(): Promise<void>
    close(): Promise<void>
    readable: ReadableStream<Uint8Array>
    writable: WritableStream<Uint8Array>
  }
  private ctl!: ReadableStreamDefaultController<Uint8Array>
  private pushSeq = 0

  constructor() {
    const dec = new Decoder()
    this.port = {
      getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
      setSignals: async () => {},
      open: async () => {},
      close: async () => {},
      readable: new ReadableStream<Uint8Array>({ start: (c) => void (this.ctl = c) }),
      writable: new WritableStream<Uint8Array>({
        write: (chunk) => {
          for (const f of dec.push(chunk)) {
            this.requests.push(f)
            this.answer(f)
          }
        },
      }),
    }
  }

  /** A Serial whose granted port is this firmware, so RealDevice auto-connects to it. */
  serial(): Serial {
    return {
      addEventListener: () => {},
      removeEventListener: () => {},
      getPorts: async () => [this.port],
      requestPort: async () => this.port,
    } as unknown as Serial
  }

  cmds(): number[] {
    return this.requests.map((r) => r.cmd).filter((c) => c !== Cmd.Ping)
  }

  pushResults(frameId: number, functionId: number, records: FakeRecord[]) {
    this.send(Cmd.PushResults, this.pushSeq++ & 0xff, encodeResults(frameId, functionId, records))
  }

  pushFrame(frameId: number) {
    if ((this.streamFlags & 1) === 0) return
    const p = new Uint8Array(12 + 4)
    const v = new DataView(p.buffer)
    v.setUint32(0, frameId, true)
    v.setUint16(8, 320, true)
    v.setUint16(10, 240, true)
    p.set([0xff, 0xd8, 0xff, 0xd9], 12)
    this.send(Cmd.PushFrame, this.pushSeq++ & 0xff, p)
  }

  /** The R4 (over UART/I2C) changes the function. */
  externalSetFunction(fn: number) {
    this.active = fn
  }

  close() {
    this.ctl.close()
  }

  private send(cmd: number, seq: number, payload: Uint8Array) {
    this.ctl.enqueue(encodeFrame(cmd, seq, payload))
  }

  private reply(f: Frame, r: Reply) {
    if (r === null) return
    const delay = this.replyDelay.get(f.cmd)
    if (delay) {
      setTimeout(() => this.replyNow(f, r), delay)
      return
    }
    this.replyNow(f, r)
  }

  private replyNow(f: Frame, r: Uint8Array | { error: number }) {
    if (r instanceof Uint8Array) this.send(responseOf(f.cmd), f.seq, r)
    else this.send(Cmd.Error, f.seq, new Uint8Array([f.cmd, r.error]))
  }

  private learnedPayload(fn: number): Uint8Array {
    const items: { id: number; rgb: number[]; name: Uint8Array }[] = []
    const enc = new TextEncoder()
    if (fn === 1) {
      for (const c of this.colors) items.push({ id: c.id, rgb: c.rgb, name: enc.encode(this.names.get(`1:${c.id}`) ?? '') })
    } else {
      for (const [k, n] of this.names) {
        const [f, id] = k.split(':').map(Number)
        if (f === fn) items.push({ id, rgb: [0, 0, 0], name: enc.encode(n) })
      }
      items.sort((a, b) => a.id - b.id)
    }
    const out = new Uint8Array(1 + items.reduce((n, i) => n + 6 + i.name.length, 0))
    const v = new DataView(out.buffer)
    out[0] = items.length
    let o = 1
    for (const i of items) {
      v.setUint16(o, i.id, true)
      out.set(i.rgb, o + 2)
      out[o + 5] = i.name.length
      out.set(i.name, o + 6)
      o += 6 + i.name.length
    }
    return out
  }

  private answer(f: Frame) {
    const dropping = this.drop.get(f.cmd) ?? 0
    if (dropping > 0) {
      this.drop.set(f.cmd, dropping - 1)
      return
    }
    if (this.unknown.has(f.cmd)) return this.reply(f, { error: ErrorCode.UnknownCmd })
    const forced = this.failWith.get(f.cmd)
    if (forced !== undefined) return this.reply(f, { error: forced })
    const p = f.payload
    const v = new DataView(p.buffer, p.byteOffset, p.byteLength)
    switch (f.cmd) {
      case Cmd.GetInfo:
        return this.reply(f, infoPayload(this.functionIds))
      case Cmd.StreamStart:
        this.streamFlags = p[0] ?? 0
        return this.reply(f, new Uint8Array(0))
      case Cmd.Ping:
      case Cmd.StreamStop:
        return this.reply(f, new Uint8Array(0))
      case Cmd.GetFuncConfig:
        if (p.length !== 1) return this.reply(f, { error: ErrorCode.BadLength })
        if (!this.hasFuncConfig(p[0])) return this.reply(f, { error: ErrorCode.BadParam })
        return this.reply(f, this.gesturePayload())
      case Cmd.SetFuncConfig: {
        if (p.length !== 10) return this.reply(f, { error: ErrorCode.BadLength })
        if (!this.hasFuncConfig(p[0]) || p[1] !== 1) return this.reply(f, { error: ErrorCode.BadParam })
        const g = this.gesture
        const next = {
          handThreshold: clampN(p[2], 10, 95),
          gestureThreshold: Math.min(100, p[3]),
          maxHands: clampN(p[4], 1, 2),
        }
        if (JSON.stringify(next) !== JSON.stringify(g)) this.config.dirty = true
        this.gesture = next
        return this.reply(f, this.gesturePayload())
      }
      case Cmd.SetFunction:
        if (this.failSetFunction > 0) {
          this.failSetFunction--
          return this.reply(f, { error: ErrorCode.Internal })
        }
        if (p[0] !== 0 && !this.functionIds.includes(p[0])) return this.reply(f, { error: ErrorCode.BadParam })
        if (p[0] !== this.active && this.pendingLearn) {
          // a function switch cancels a pending LEARN, which then answers BAD_PARAM
          clearTimeout(this.pendingLearn.timer)
          this.reply(this.pendingLearn.f, { error: ErrorCode.BadParam })
          this.pendingLearn = null
        }
        this.active = p[0]
        return this.reply(f, new Uint8Array([p[0]]))
      case Cmd.GetFunction:
        return this.reply(f, new Uint8Array([this.active]))
      case Cmd.GetConfig:
        return this.reply(f, this.configPayload())
      case Cmd.SetConfig: {
        const c = parseConfig(p)
        // clamp like the firmware
        c.tolerance = Math.min(100, c.tolerance)
        c.colorMaxResults = Math.min(8, Math.max(1, c.colorMaxResults))
        c.faceThreshold = Math.min(100, c.faceThreshold)
        c.faceMaxResults = Math.min(8, Math.max(1, c.faceMaxResults))
        c.ledBrightness = Math.min(100, c.ledBrightness)
        if (c.i2cAddress < 0x08 || c.i2cAddress > 0x77) c.i2cAddress = 0x5a
        const changed = JSON.stringify({ ...c, dirty: false }) !== JSON.stringify({ ...this.config, dirty: false })
        this.config = { ...c, dirty: this.config.dirty || changed }
        return this.reply(f, this.configPayload())
      }
      case Cmd.GetLearned:
        if (p[0] !== 1 && p[0] !== 2 && !(p[0] === GESTURE_FN && this.functionIds.includes(GESTURE_FN)))
          return this.reply(f, { error: ErrorCode.BadParam })
        return this.reply(f, this.learnedPayload(p[0]))
      case Cmd.Learn: {
        // like the firmware: only the colour function learns, and only while it is the active one
        if (p[0] !== 1 || this.active !== 1) return this.reply(f, { error: ErrorCode.BadParam })
        if (this.pendingLearn) return this.reply(f, { error: ErrorCode.Busy })
        const timer = setTimeout(() => {
          this.pendingLearn = null
          if (this.learnReply) return this.reply(f, this.learnReply)
          const rgb = this.learnRgb
          if (!rgb) return this.reply(f, { error: ErrorCode.BadParam })
          const same = this.colors.find((c) => c.rgb.join() === rgb.join())
          if (same) return this.reply(f, new Uint8Array([same.id]))
          let id = 1
          while (this.colors.some((c) => c.id === id)) id++
          if (id > 7) return this.reply(f, { error: ErrorCode.Full })
          this.colors.push({ id, rgb })
          this.colors.sort((a, b) => a.id - b.id)
          this.config.dirty = true
          this.reply(f, new Uint8Array([id]))
        }, this.learnDelayMs)
        this.pendingLearn = { f, timer }
        return
      }
      case Cmd.Forget: {
        const fn = p[0]
        const id = v.getUint16(1, true)
        // face (3) has nothing to forget: answers OK like firmware (protocol.md 0x31)
        if (fn !== 1 && fn !== 2 && fn !== 3 && !this.hasGestureNames(fn)) return this.reply(f, { error: ErrorCode.BadParam })
        const before = this.colors.length + this.names.size // like firmware: dirty only if something was removed
        if (fn === 1) this.colors = id === 0xffff ? [] : this.colors.filter((c) => c.id !== id)
        for (const k of [...this.names.keys()]) if (k.startsWith(`${fn}:`) && (id === 0xffff || k === `${fn}:${id}`)) this.names.delete(k)
        if (this.colors.length + this.names.size !== before) this.config.dirty = true
        return this.reply(f, new Uint8Array(0))
      }
      case Cmd.SetName: {
        const fn = p[0]
        const id = v.getUint16(1, true)
        const name = new TextDecoder().decode(p.subarray(4, 4 + p[3]))
        if (fn !== 1 && fn !== 2 && !this.hasGestureNames(fn)) return this.reply(f, { error: ErrorCode.BadParam }) // face has no names
        if (fn === GESTURE_FN && (id < 1 || id > 9)) return this.reply(f, { error: ErrorCode.BadParam }) // 9 fixed gestures
        if (fn === 1 && !this.colors.some((c) => c.id === id)) return this.reply(f, { error: ErrorCode.BadParam })
        if (name === '') this.names.delete(`${fn}:${id}`)
        else {
          if (!this.names.has(`${fn}:${id}`) && this.names.size >= 32) return this.reply(f, { error: ErrorCode.Full })
          this.names.set(`${fn}:${id}`, name)
        }
        this.config.dirty = true
        return this.reply(f, new Uint8Array(0))
      }
      case Cmd.Save:
        setTimeout(() => {
          this.config.dirty = false
          this.reply(f, new Uint8Array(0))
        }, this.saveDelayMs)
        return
      case Cmd.FactoryReset:
        setTimeout(() => {
          this.config = { ...DEFAULT_CONFIG }
          this.gesture = { ...DEFAULT_GESTURE }
          this.colors = []
          this.names.clear()
          this.reply(f, new Uint8Array(0))
        }, this.saveDelayMs)
        return
      default:
        return this.reply(f, { error: ErrorCode.UnknownCmd })
    }
  }

  private hasFuncConfig(fn: number): boolean {
    return fn === GESTURE_FN && this.functionIds.includes(GESTURE_FN)
  }
  private hasGestureNames(fn: number): boolean {
    return fn === GESTURE_FN && this.functionIds.includes(GESTURE_FN)
  }
  private gesturePayload(): Uint8Array {
    const g = this.gesture
    return new Uint8Array([GESTURE_FN, 1, g.handThreshold, g.gestureThreshold, g.maxHands, 0, 0, 0, 0, 0])
  }

  private configPayload(): Uint8Array {
    const out = encodeConfig(this.config)
    out[15] = this.config.dirty ? 1 : 0
    return out
  }
}
