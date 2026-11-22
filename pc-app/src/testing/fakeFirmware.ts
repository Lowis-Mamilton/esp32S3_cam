// Test helper: a fake board that behaves like the M2 firmware (docs/protocol.md §3, §6, §7), reachable through
// a fake Web Serial port (serial()) or a fake Web Bluetooth device (bluetooth()).
import { Decoder, encodeFrame, type Frame } from '../protocol/codec'
import { Cmd, ErrorCode, encodeConfig, parseConfig, responseOf, type DeviceConfig } from '../protocol/messages'
import type { BluetoothCharacteristicLike, BluetoothDeviceLike, BluetoothLike } from '../transport/webBluetooth'

// The Bluetooth contract with the firmware, written out again here on purpose: a wrong constant in
// src/transport/webBluetooth.ts must fail the tests.
const BLE_SERVICE = '4d584300-0010-4b1e-9c5a-7f3e2a1d0c9b'
const BLE_RX = '4d584300-0011-4b1e-9c5a-7f3e2a1d0c9b'
const BLE_TX = '4d584300-0012-4b1e-9c5a-7f3e2a1d0c9b'

/** Gesture config v1 values (docs/protocol.md section 10). */
export interface FakeGestureConfig {
  handThreshold: number
  gestureThreshold: number
  maxHands: number
}
export const DEFAULT_GESTURE: FakeGestureConfig = { handThreshold: 40, gestureThreshold: 60, maxHands: 1 }
const GESTURE_FN = 8
const LINE_FN = 4
const TRACK_FN = 5

/** Line config v1 / Track config v1 values (docs/protocol.md section 9). */
export interface FakeLineConfig {
  mode: number
  threshold: number
  roiPct: number
  bands: number
  minWidth: number
  maxWidth: number
  colorTolerance: number
}
export const DEFAULT_LINE: FakeLineConfig = { mode: 0, threshold: 0, roiPct: 60, bands: 8, minWidth: 4, maxWidth: 80, colorTolerance: 30 }
export interface FakeTrackConfig {
  threshold: number
  adapt: number
  reacquire: number
  scale: number
}
export const DEFAULT_TRACK: FakeTrackConfig = { threshold: 50, adapt: 10, reacquire: 1, scale: 1 }
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
  /** Line extra (function 4): 14 bytes. */
  line?: { angleDeg: number; offsetX: number; start: [number, number]; end: [number, number]; flags?: number }
}

export function encodeResults(frameId: number, functionId: number, records: FakeRecord[]): Uint8Array {
  const size = 6 + records.reduce((n, r) => n + 12 + (r.tag ? 18 : 0) + (r.line ? 14 : 0), 0)
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
    out[o + 11] = r.tag ? 18 : r.line ? 14 : 0
    o += 12
    if (r.line) {
      v.setInt16(o, Math.round(r.line.angleDeg * 100), true)
      v.setInt16(o + 2, r.line.offsetX, true)
      v.setInt16(o + 4, r.line.start[0], true)
      v.setInt16(o + 6, r.line.start[1], true)
      v.setInt16(o + 8, r.line.end[0], true)
      v.setInt16(o + 10, r.line.end[1], true)
      out[o + 12] = r.line.flags ?? 0
      o += 14
    }
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
  /** layout_version byte of the gesture func config replies. */
  gestureLayoutVersion = 1
  /** Line config v1 (function 4; GET/SET_FUNC_CONFIG). Add 4 to functionIds to enable it. */
  line: FakeLineConfig = { ...DEFAULT_LINE }
  lineLayoutVersion = 1
  /** Track config v1 (function 5). Add 5 to functionIds to enable it. */
  track: FakeTrackConfig = { ...DEFAULT_TRACK }
  trackLayoutVersion = 1
  /** Learned line colour (LEARN 4); null = none. */
  lineColor: [number, number, number] | null = null
  /** Learned tracking object (LEARN 5, id 1); null = none. Its name lives in `names` ("5:1"). */
  trackObject: { rgb: [number, number, number] } | null = null
  /** LEARN 5 succeeds; false = region too small or without texture (BAD_PARAM). */
  learnObjectOk = true
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

  // ---- Bluetooth ----
  /** What the device chooser does: hands over this board, is closed by the user, or finds no adapter. */
  btPicker: 'device' | 'cancel' | 'no-adapter' = 'device'
  /** false: the board has no MXCam Bluetooth service (firmware without Bluetooth). */
  btHasService = true
  /** A notification carries at most this many bytes, so packets arrive in pieces. */
  btNotifyChunk = 20
  /** Length of every write to the RX characteristic, in order. */
  btWrites: number[] = []
  /** Most RX writes ever in progress at once; Web Bluetooth allows one. */
  btMaxConcurrentWrites = 0
  /** Options of the last requestDevice() call. */
  btRequestOptions: unknown
  /** true while a GATT connection is up. */
  btConnected = false
  private btNotify: ((bytes: Uint8Array) => void) | null = null
  private btDropLink: (() => void) | null = null

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

  /** A navigator.bluetooth whose device chooser offers this firmware (see btPicker). */
  bluetooth(): BluetoothLike {
    const valueListeners = new Set<(ev: Event) => void>()
    const lostListeners = new Set<() => void>()
    let dec = new Decoder()
    let notifying = false
    let inFlight = 0
    const tx: BluetoothCharacteristicLike = {
      value: null,
      startNotifications: async () => {
        notifying = true
      },
      writeValueWithResponse: async () => {
        throw new DOMException('GATT operation not permitted.', 'NotSupportedError')
      },
      addEventListener: (_type, l) => void valueListeners.add(l),
      removeEventListener: (_type, l) => void valueListeners.delete(l),
    }
    const rx: BluetoothCharacteristicLike = {
      value: null,
      startNotifications: async () => {},
      writeValueWithResponse: async (data) => {
        if (!this.btConnected) throw new DOMException('GATT Server is disconnected.', 'NetworkError')
        inFlight++
        this.btMaxConcurrentWrites = Math.max(this.btMaxConcurrentWrites, inFlight)
        await Promise.resolve() // a real write takes a round trip: overlapping writes would show up here
        inFlight--
        this.btWrites.push(data.length)
        for (const f of dec.push(data)) {
          this.requests.push(f)
          this.answer(f)
        }
      },
      addEventListener: () => {},
      removeEventListener: () => {},
    }
    const down = () => {
      this.btConnected = false
      this.btNotify = null
      this.btDropLink = null
      notifying = false
    }
    const device: BluetoothDeviceLike = {
      name: 'MXCam-0506',
      gatt: {
        connect: async () => {
          dec = new Decoder()
          this.btConnected = true
          this.btNotify = (bytes) => {
            if (!notifying) return
            for (let off = 0; off < bytes.length; off += this.btNotifyChunk) {
              const piece = bytes.slice(off, off + this.btNotifyChunk)
              tx.value = new DataView(piece.buffer)
              for (const l of [...valueListeners]) l({ target: tx } as unknown as Event)
            }
          }
          this.btDropLink = () => {
            down()
            for (const l of [...lostListeners]) l()
          }
          return {
            getPrimaryService: async (uuid: string) => {
              if (!this.btHasService || uuid !== BLE_SERVICE) throw new DOMException('No Services matching UUID found in Device.', 'NotFoundError')
              return {
                getCharacteristic: async (u: string) => {
                  if (u === BLE_RX) return rx
                  if (u === BLE_TX) return tx
                  throw new DOMException('No Characteristics matching UUID found in Service.', 'NotFoundError')
                },
              }
            },
          }
        },
        disconnect: down,
      },
      addEventListener: (_type, l) => void lostListeners.add(l),
      removeEventListener: (_type, l) => void lostListeners.delete(l),
    }
    return {
      requestDevice: async (options) => {
        this.btRequestOptions = options
        if (this.btPicker === 'cancel') throw new DOMException('User cancelled the requestDevice() chooser.', 'NotFoundError')
        if (this.btPicker === 'no-adapter') throw new DOMException('Bluetooth adapter not available.', 'NotFoundError')
        return device
      },
    }
  }

  /** The Bluetooth link drops on its own (board out of range or switched off). */
  btDrop() {
    this.btDropLink?.()
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

  /** Replies and pushes go out on the Bluetooth link while one is up, otherwise on the serial port. */
  private send(cmd: number, seq: number, payload: Uint8Array) {
    const bytes = encodeFrame(cmd, seq, payload)
    if (this.btNotify) this.btNotify(bytes)
    else this.ctl.enqueue(bytes)
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
    if (fn === LINE_FN) {
      if (this.lineColor) items.push({ id: 1, rgb: this.lineColor, name: enc.encode('') })
    } else if (fn === 1) {
      for (const c of this.colors) items.push({ id: c.id, rgb: c.rgb, name: enc.encode(this.names.get(`1:${c.id}`) ?? '') })
    } else if (fn === TRACK_FN) {
      if (this.trackObject) items.push({ id: 1, rgb: this.trackObject.rgb, name: enc.encode(this.names.get(`${TRACK_FN}:1`) ?? '') })
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
        return this.reply(f, this.funcPayload(p[0]))
      case Cmd.SetFuncConfig: {
        if (p.length !== 10) return this.reply(f, { error: ErrorCode.BadLength })
        if (!this.hasFuncConfig(p[0]) || p[1] !== this.layoutVersion(p[0])) return this.reply(f, { error: ErrorCode.BadParam })
        const before = JSON.stringify(this.funcState(p[0]))
        this.applyFuncConfig(p)
        if (JSON.stringify(this.funcState(p[0])) !== before) this.config.dirty = true
        return this.reply(f, this.funcPayload(p[0]))
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
        if (p[0] !== 1 && p[0] !== 2 && !this.hasFuncFn(p[0])) return this.reply(f, { error: ErrorCode.BadParam })
        return this.reply(f, this.learnedPayload(p[0]))
      case Cmd.Learn: {
        // like the firmware: only the colour function learns, and only while it is the active one
        const learnable = p[0] === 1 || (this.hasFuncFn(p[0]) && (p[0] === LINE_FN || p[0] === TRACK_FN))
        if (!learnable || this.active !== p[0] || p.length !== 9) return this.reply(f, { error: ErrorCode.BadParam })
        if (this.pendingLearn) return this.reply(f, { error: ErrorCode.Busy })
        const timer = setTimeout(() => {
          this.pendingLearn = null
          if (this.learnReply) return this.reply(f, this.learnReply)
          const rgb = this.learnRgb
          if (p[0] === TRACK_FN) {
            if (!this.learnObjectOk) return this.reply(f, { error: ErrorCode.BadParam })
            this.trackObject = { rgb: rgb ?? [128, 128, 128] }
            this.names.delete(`${TRACK_FN}:1`)
            this.config.dirty = true
            return this.reply(f, new Uint8Array([1]))
          }
          if (!rgb) return this.reply(f, { error: ErrorCode.BadParam })
          if (p[0] === LINE_FN) {
            this.lineColor = rgb
            this.line.mode = 2
            this.config.dirty = true
            return this.reply(f, new Uint8Array([1]))
          }
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
        if (fn !== 1 && fn !== 2 && fn !== 3 && !this.hasFuncFn(fn)) return this.reply(f, { error: ErrorCode.BadParam })
        const count = () => this.colors.length + this.names.size + (this.lineColor ? 1 : 0) + (this.trackObject ? 1 : 0)
        const modeBefore = this.line.mode
        const before = count() // like firmware: dirty only if something was removed
        if (fn === LINE_FN && (id === 1 || id === 0xffff)) {
          this.lineColor = null
          // like firmware (mxst_set_line_color): only mode 2 (learned) falls back to 0; mode 1 is kept
          if (this.line.mode === 2) this.line.mode = 0
        }
        if (fn === TRACK_FN && (id === 1 || id === 0xffff)) this.trackObject = null
        if (fn === 1) this.colors = id === 0xffff ? [] : this.colors.filter((c) => c.id !== id)
        for (const k of [...this.names.keys()]) if (k.startsWith(`${fn}:`) && (id === 0xffff || k === `${fn}:${id}`)) this.names.delete(k)
        if (count() !== before || this.line.mode !== modeBefore) this.config.dirty = true
        return this.reply(f, new Uint8Array(0))
      }
      case Cmd.SetName: {
        const fn = p[0]
        const id = v.getUint16(1, true)
        const name = new TextDecoder().decode(p.subarray(4, 4 + p[3]))
        if (fn !== 1 && fn !== 2 && !this.hasFuncFn(fn)) return this.reply(f, { error: ErrorCode.BadParam }) // face has no names
        if (fn === LINE_FN) return this.reply(f, { error: ErrorCode.BadParam }) // the line cannot be named
        if (fn === TRACK_FN && !(id === 1 && this.trackObject)) return this.reply(f, { error: ErrorCode.BadParam })
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
          this.line = { ...DEFAULT_LINE }
          this.track = { ...DEFAULT_TRACK }
          this.lineColor = null
          this.trackObject = null
          this.colors = []
          this.names.clear()
          this.reply(f, new Uint8Array(0))
        }, this.saveDelayMs)
        return
      default:
        return this.reply(f, { error: ErrorCode.UnknownCmd })
    }
  }

  /** Functions with a func config and learned data beyond colour/tag/face, as listed by GET_INFO. */
  private hasFuncFn(fn: number): boolean {
    return (fn === GESTURE_FN || fn === LINE_FN || fn === TRACK_FN) && this.functionIds.includes(fn)
  }
  private hasFuncConfig(fn: number): boolean {
    return this.hasFuncFn(fn)
  }
  private layoutVersion(fn: number): number {
    return fn === GESTURE_FN ? this.gestureLayoutVersion : fn === LINE_FN ? this.lineLayoutVersion : this.trackLayoutVersion
  }
  private funcState(fn: number) {
    return fn === GESTURE_FN ? this.gesture : fn === LINE_FN ? this.line : this.track
  }
  private applyFuncConfig(p: Uint8Array) {
    if (p[0] === GESTURE_FN) {
      this.gesture = { handThreshold: clampN(p[2], 10, 95), gestureThreshold: Math.min(100, p[3]), maxHands: clampN(p[4], 1, 2) }
    } else if (p[0] === LINE_FN) {
      const minWidth = clampN(p[6], 1, 100)
      this.line = {
        mode: p[2] > 2 ? 0 : p[2],
        threshold: p[3],
        roiPct: clampN(p[4], 20, 100),
        bands: clampN(p[5], 4, 12),
        minWidth,
        maxWidth: Math.max(minWidth, p[7]),
        colorTolerance: Math.min(100, p[8]),
      }
    } else {
      this.track = { threshold: clampN(p[2], 10, 95), adapt: clampN(p[3], 0, 50), reacquire: p[4] ? 1 : 0, scale: p[5] ? 1 : 0 }
    }
  }
  private funcPayload(fn: number): Uint8Array {
    if (fn === LINE_FN) {
      const l = this.line
      return new Uint8Array([fn, this.lineLayoutVersion, l.mode, l.threshold, l.roiPct, l.bands, l.minWidth, l.maxWidth, l.colorTolerance, 0])
    }
    if (fn === TRACK_FN) {
      const t = this.track
      return new Uint8Array([fn, this.trackLayoutVersion, t.threshold, t.adapt, t.reacquire, t.scale, 0, 0, 0, 0])
    }
    const g = this.gesture
    return new Uint8Array([GESTURE_FN, this.gestureLayoutVersion, g.handThreshold, g.gestureThreshold, g.maxHands, 0, 0, 0, 0, 0])
  }

  private configPayload(): Uint8Array {
    const out = encodeConfig(this.config)
    out[15] = this.config.dirty ? 1 : 0
    return out
  }
}
