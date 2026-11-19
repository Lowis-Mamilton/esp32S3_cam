// Web Serial connection to an MXCam. Protocol: docs/protocol.md.
import { Decoder, encodeFrame, type Frame } from '../protocol/codec'
import {
  Cmd,
  encodeStreamStart,
  parseError,
  parseFramePush,
  parseInfo,
  parseResults,
  responseOf,
  type DeviceInfo,
  type FramePush,
  type ResultsPush,
} from '../protocol/messages'

export type ConnectionStatus = 'unsupported' | 'disconnected' | 'connecting' | 'connected'

export interface ConnectionEvents {
  status(s: ConnectionStatus): void
  frame(f: FramePush): void
  stats(s: { fps: number; kbps: number }): void
  /** Optional: called with the GET_INFO result after each successful connect. */
  info?(i: DeviceInfo): void
  /** Optional: runs after GET_INFO and before STREAM_START (status is still 'connecting'); a rejection fails the connect. */
  setup?(i: DeviceInfo): Promise<void>
  /** Optional: RESULTS pushes (0x61); only sent by the device when streamResults is on. */
  results?(r: ResultsPush): void
}

export interface ConnectionOptions {
  /** Subscribe to RESULTS pushes (STREAM_START flags bit1). Default false. */
  streamResults?: boolean
}

/** Device answered a request with an ERROR frame (protocol.md §3). */
export class MxcamError extends Error {
  readonly cmd: number
  readonly code: number
  constructor(cmd: number, code: number) {
    super(`device error: cmd 0x${cmd.toString(16)} code ${code}`)
    this.name = 'MxcamError'
    this.cmd = cmd
    this.code = code
  }
}

interface Pending {
  cmd: number
  resolve(f: Frame): void
  reject(e: Error): void
  timer: ReturnType<typeof setTimeout>
}

interface Session {
  port: SerialPort
  reader: ReadableStreamDefaultReader<Uint8Array>
  writer: WritableStreamDefaultWriter<Uint8Array>
  decoder: Decoder
  loopDone: Promise<void>
}

// Native USB-Serial-JTAG of the ESP32-S3 (the OTG port on the board).
const MXCAM_USB_VID = 0x303a
const MXCAM_USB_PID = 0x1001
const MAX_PING_FAILURES = 3 // matches the firmware's 3 s stream timeout
const PING_INTERVAL_MS = 1000
const STATS_INTERVAL_MS = 1000
const WATCHDOG_INTERVAL_MS = 500
const WATCHDOG_SILENT_TICKS = 5 // 5 x 500 ms = 2.5 s without a FRAME push
// Cold boot: the device may still be enumerating/booting, so GET_INFO is retried
// (1 s per attempt, 250 ms gap, 4 attempts ~= 4.75 s). Used by both connect paths.
const GET_INFO_ATTEMPTS = 4
const GET_INFO_TIMEOUT_MS = 1000
const GET_INFO_GAP_MS = 250
const STOP_ON_DISCONNECT_TIMEOUT_MS = 300

export class MxcamConnection {
  private readonly serial: Serial | undefined
  private readonly events: ConnectionEvents
  private readonly streamResults: boolean
  private status: ConnectionStatus
  private session: Session | null = null
  private cleanup: Promise<void> = Promise.resolve()
  private opening: Promise<unknown> | null = null
  private readonly pending = new Map<number, Pending>()
  private seq = 0
  private pingTimer: ReturnType<typeof setInterval> | null = null
  private statsTimer: ReturnType<typeof setInterval> | null = null
  private watchdogTimer: ReturnType<typeof setInterval> | null = null
  private streaming = false
  private streamPayload: Uint8Array | null = null
  private silentTicks = 0
  private frameCount = 0
  private byteCount = 0

  private readonly onSerialConnect = (ev: Event) => {
    if (this.status !== 'disconnected' || this.opening) return
    const t = ev.target as SerialPort | null
    void this.autoConnect(t && this.isMxcam(t) ? t : undefined)
  }
  private readonly onVisibility = () => {
    if (typeof document !== 'undefined' && document.visibilityState === 'visible') this.resendStreamStart()
  }
  private readonly onSerialDisconnect = (ev: Event) => {
    const s = this.session
    // Real events target the port; ignore ones for other ports.
    if (s && ev.target === s.port) {
      void this.teardown(s, new Error('device disconnected'))
    }
  }

  constructor(serial: Serial | undefined, events: ConnectionEvents, options: ConnectionOptions = {}) {
    this.serial = serial
    this.events = events
    this.streamResults = options.streamResults ?? false
    if (!serial) {
      this.status = 'unsupported'
      events.status('unsupported')
      return
    }
    this.status = 'disconnected'
    serial.addEventListener('connect', this.onSerialConnect)
    serial.addEventListener('disconnect', this.onSerialDisconnect)
    if (typeof document !== 'undefined') document.addEventListener('visibilitychange', this.onVisibility)
  }

  /** Remove serial listeners and close the port. */
  dispose(): Promise<void> {
    this.serial?.removeEventListener('connect', this.onSerialConnect)
    this.serial?.removeEventListener('disconnect', this.onSerialDisconnect)
    if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', this.onVisibility)
    return this.disconnect()
  }

  private setStatus(s: ConnectionStatus) {
    if (this.status === s) return
    this.status = s
    this.events.status(s)
  }

  private isMxcam(port: SerialPort): boolean {
    const i = port.getInfo()
    return i.usbVendorId === MXCAM_USB_VID && i.usbProductId === MXCAM_USB_PID
  }

  /** Run `fn` as the single in-flight open; the flag is set synchronously. */
  private async exclusive<T>(fn: () => Promise<T>): Promise<T> {
    const p = fn()
    this.opening = p
    try {
      return await p
    } finally {
      if (this.opening === p) this.opening = null
    }
  }

  async connect(): Promise<void> {
    if (!this.serial || this.opening || this.status === 'connecting' || this.status === 'connected') return
    const serial = this.serial
    await this.exclusive(async () => {
      let port: SerialPort
      try {
        port = await serial.requestPort({ filters: [{ usbVendorId: MXCAM_USB_VID, usbProductId: MXCAM_USB_PID }] })
      } catch (e) {
        if (e instanceof DOMException && e.name === 'NotFoundError') return // user cancelled the picker
        throw e
      }
      await this.openPort(port)
    })
  }

  /** Returns false when already opening/connected or no matching granted port exists. */
  async autoConnect(preferred?: SerialPort): Promise<boolean> {
    if (!this.serial || this.opening || this.status === 'connecting' || this.status === 'connected') return false
    const serial = this.serial
    try {
      return await this.exclusive(async () => {
        const candidates = preferred ? [preferred] : (await serial.getPorts()).filter((p) => this.isMxcam(p))
        for (const port of candidates) {
          try {
            await this.openPort(port)
            return true
          } catch {
            // try the next granted port
          }
        }
        return false
      })
    } catch {
      return false
    }
  }

  private async openPort(port: SerialPort): Promise<void> {
    await this.cleanup
    this.setStatus('connecting')
    let s: Session | null = null
    try {
      await port.open({ baudRate: 115200 }) // baud is ignored by USB-Serial-JTAG but required by the API
      // The ESP32-S3 USB-Serial-JTAG treats DTR/RTS changes as reset/bootloader control, and
      // closing the port re-asserts them (RTS=1 with DTR=0 resets the chip). Drop RTS first,
      // then DTR, as two separate calls so the lines never pass through DTR=0/RTS=1.
      try {
        await port.setSignals({ requestToSend: false })
        await port.setSignals({ dataTerminalReady: false })
      } catch (e) {
        console.warn('setSignals failed, continuing', e)
      }
      if (!port.readable || !port.writable) throw new Error('port streams unavailable')
      const created: Session = {
        port,
        reader: port.readable.getReader(),
        writer: port.writable.getWriter(),
        decoder: new Decoder(),
        loopDone: Promise.resolve(),
      }
      s = created
      this.session = created
      created.loopDone = this.readLoop(created)
      let infoFrame: Frame | undefined
      for (let attempt = 1; !infoFrame; attempt++) {
        try {
          infoFrame = await this.request(Cmd.GetInfo, new Uint8Array(0), GET_INFO_TIMEOUT_MS)
        } catch (e) {
          if (this.session !== created || attempt >= GET_INFO_ATTEMPTS || e instanceof MxcamError) throw e
          await new Promise((r) => setTimeout(r, GET_INFO_GAP_MS))
          if (this.session !== created) throw new Error('disconnected during setup')
        }
      }
      const info = parseInfo(infoFrame.payload)
      this.events.info?.(info)
      if (this.events.setup) {
        await this.events.setup(info)
        if (this.session !== created) throw new Error('disconnected during setup')
      }
      await this.startStream()
      if (this.session !== created) throw new Error('disconnected during setup')
      this.setStatus('connected')
    } catch (e) {
      if (s) await this.teardown(s, e instanceof Error ? e : new Error(String(e)))
      else {
        await port.close().catch(() => {})
        this.setStatus('disconnected')
      }
      throw e
    }
  }

  private async readLoop(s: Session): Promise<void> {
    try {
      for (;;) {
        const { value, done } = await s.reader.read()
        if (done) break
        if (value) {
          for (const f of s.decoder.push(value)) {
            try {
              this.handleFrame(f)
            } catch {
              // malformed payload: drop it, keep reading
            }
          }
        }
      }
    } catch {
      // read error (e.g. device unplugged) is treated like end of stream
    } finally {
      try {
        s.reader.releaseLock()
      } catch {
        // ignore
      }
    }
    void this.teardown(s, new Error('connection closed'))
  }

  private handleFrame(f: Frame) {
    if (f.cmd === Cmd.PushFrame) {
      const fp = parseFramePush(f.payload)
      this.silentTicks = 0
      this.frameCount++
      this.byteCount += fp.jpeg.length
      this.events.frame(fp)
      return
    }
    if (f.cmd === Cmd.PushResults) {
      this.events.results?.(parseResults(f.payload))
      return
    }
    const p = this.pending.get(f.seq)
    if (!p) return // late/unknown response
    if (f.cmd === Cmd.Error) {
      const e = parseError(f.payload)
      // A stale ERROR (e.g. a LEARN cancel left in the USB ring) with a recycled seq must not fail an unrelated request.
      if (e.cmd !== p.cmd) return
      this.settle(f.seq, () => p.reject(new MxcamError(e.cmd, e.code)))
    } else if (f.cmd === responseOf(p.cmd)) {
      this.settle(f.seq, () => p.resolve(f))
    }
  }

  private settle(seq: number, fn: () => void) {
    const p = this.pending.get(seq)
    if (!p) return
    clearTimeout(p.timer)
    this.pending.delete(seq)
    fn()
  }

  private rejectAll(reason: Error) {
    for (const [seq, p] of [...this.pending]) {
      this.settle(seq, () => p.reject(reason))
    }
  }

  private allocSeq(): number {
    for (let i = 0; i < 256; i++) {
      const s = (this.seq + i) & 0xff
      if (!this.pending.has(s)) {
        this.seq = (s + 1) & 0xff
        return s
      }
    }
    throw new Error('too many pending requests')
  }

  request(cmd: number, payload: Uint8Array = new Uint8Array(0), timeoutMs = 1000): Promise<Frame> {
    const s = this.session
    if (!s) return Promise.reject(new Error('not connected'))
    return new Promise<Frame>((resolve, reject) => {
      let seq: number
      try {
        seq = this.allocSeq()
      } catch (e) {
        reject(e)
        return
      }
      const timer = setTimeout(() => {
        this.settle(seq, () => reject(new Error(`request 0x${cmd.toString(16)} timeout`)))
      }, timeoutMs)
      this.pending.set(seq, { cmd, resolve, reject, timer })
      s.writer.write(encodeFrame(cmd, seq, payload)).catch((e: unknown) => {
        this.settle(seq, () => reject(e instanceof Error ? e : new Error(String(e))))
      })
    })
  }

  async startStream(maxFps = 10): Promise<void> {
    const payload = encodeStreamStart({ frames: true, results: this.streamResults, maxFps, jpegQuality: 0 })
    await this.request(Cmd.StreamStart, payload)
    this.stopTimers()
    this.streaming = true
    this.streamPayload = payload
    this.silentTicks = 0
    this.frameCount = 0
    this.byteCount = 0
    // Firmware stops streaming after 3 s without a request (protocol.md §6).
    const session = this.session
    let failures = 0
    this.pingTimer = setInterval(() => {
      this.request(Cmd.Ping).then(
        () => {
          failures = 0
        },
        () => {
          if (++failures >= MAX_PING_FAILURES && session && this.session === session) {
            void this.teardown(session, new Error('device not responding'))
          }
        },
      )
    }, PING_INTERVAL_MS)
    // A repeated STREAM_START replaces parameters (protocol.md §6) and restarts a stalled stream.
    this.watchdogTimer = setInterval(() => {
      if (++this.silentTicks >= WATCHDOG_SILENT_TICKS) {
        this.silentTicks = 0
        this.resendStreamStart()
      }
    }, WATCHDOG_INTERVAL_MS)
    this.statsTimer = setInterval(() => {
      const secs = STATS_INTERVAL_MS / 1000
      this.events.stats({ fps: this.frameCount / secs, kbps: this.byteCount / 1024 / secs })
      this.frameCount = 0
      this.byteCount = 0
    }, STATS_INTERVAL_MS)
  }

  private resendStreamStart() {
    if (!this.streaming || !this.streamPayload || !this.session) return
    this.request(Cmd.StreamStart, this.streamPayload).catch(() => {})
  }

  async stopStream(): Promise<void> {
    this.stopTimers()
    this.events.stats({ fps: 0, kbps: 0 })
    await this.request(Cmd.StreamStop)
  }

  private stopTimers() {
    if (this.pingTimer) clearInterval(this.pingTimer)
    if (this.statsTimer) clearInterval(this.statsTimer)
    if (this.watchdogTimer) clearInterval(this.watchdogTimer)
    this.pingTimer = null
    this.statsTimer = null
    this.watchdogTimer = null
    this.streaming = false
  }

  async disconnect(): Promise<void> {
    const s = this.session
    if (s) {
      if (this.streaming && this.session === s) {
        // Best effort: tell the device to stop; failures and timeouts are ignored.
        await this.request(Cmd.StreamStop, new Uint8Array(0), STOP_ON_DISCONNECT_TIMEOUT_MS).catch(() => {})
      }
      await this.teardown(s, new Error('disconnected by user'))
    }
    await this.cleanup
  }

  /** Idempotent per session: reject pending, stop timers, release locks, close the port. */
  private teardown(s: Session, reason: Error): Promise<void> {
    if (this.session !== s) return this.cleanup
    this.session = null
    this.stopTimers()
    this.rejectAll(reason)
    this.events.stats({ fps: 0, kbps: 0 })
    this.setStatus('disconnected')
    this.cleanup = (async () => {
      await s.reader.cancel().catch(() => {})
      await s.loopDone
      await s.writer.abort().catch(() => {}) // abort, not close: a stuck write must not hang cleanup
      try {
        s.writer.releaseLock()
      } catch {
        // ignore
      }
      await s.port.close().catch(() => {})
    })()
    return this.cleanup
  }
}
