// Web Serial connection to an MXCam. Protocol: docs/protocol.md.
import { Decoder, encodeFrame, type Frame } from '../protocol/codec'
import {
  Cmd,
  encodeStreamStart,
  parseError,
  parseFramePush,
  parseInfo,
  responseOf,
  type DeviceInfo,
  type FramePush,
} from '../protocol/messages'

export type ConnectionStatus = 'unsupported' | 'disconnected' | 'connecting' | 'connected'

export interface ConnectionEvents {
  status(s: ConnectionStatus): void
  frame(f: FramePush): void
  stats(s: { fps: number; kbps: number }): void
  /** Optional: called with the GET_INFO result after each successful connect. */
  info?(i: DeviceInfo): void
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

const PING_INTERVAL_MS = 1000
const STATS_INTERVAL_MS = 1000

export class MxcamConnection {
  private readonly serial: Serial | undefined
  private readonly events: ConnectionEvents
  private status: ConnectionStatus
  private session: Session | null = null
  private cleanup: Promise<void> = Promise.resolve()
  private readonly pending = new Map<number, Pending>()
  private seq = 0
  private pingTimer: ReturnType<typeof setInterval> | null = null
  private statsTimer: ReturnType<typeof setInterval> | null = null
  private frameCount = 0
  private byteCount = 0

  private readonly onSerialConnect = () => {
    if (this.status === 'disconnected') void this.autoConnect()
  }
  private readonly onSerialDisconnect = (ev: Event) => {
    const s = this.session
    // Real events target the port; ignore ones for other ports.
    if (s && (ev.target === s.port || ev.target === this.serial)) {
      void this.teardown(s, new Error('device disconnected'))
    }
  }

  constructor(serial: Serial | undefined, events: ConnectionEvents) {
    this.serial = serial
    this.events = events
    if (!serial) {
      this.status = 'unsupported'
      events.status('unsupported')
      return
    }
    this.status = 'disconnected'
    serial.addEventListener('connect', this.onSerialConnect)
    serial.addEventListener('disconnect', this.onSerialDisconnect)
  }

  /** Remove serial listeners and close the port. */
  dispose(): Promise<void> {
    this.serial?.removeEventListener('connect', this.onSerialConnect)
    this.serial?.removeEventListener('disconnect', this.onSerialDisconnect)
    return this.disconnect()
  }

  private setStatus(s: ConnectionStatus) {
    if (this.status === s) return
    this.status = s
    this.events.status(s)
  }

  async connect(): Promise<void> {
    if (!this.serial || this.status === 'connecting' || this.status === 'connected') return
    let port: SerialPort
    try {
      port = await this.serial.requestPort()
    } catch (e) {
      if (e instanceof DOMException && e.name === 'NotFoundError') return // user cancelled the picker
      throw e
    }
    await this.openPort(port)
  }

  async autoConnect(): Promise<boolean> {
    if (!this.serial || this.status === 'connecting' || this.status === 'connected') return false
    try {
      const ports = await this.serial.getPorts()
      if (ports.length === 0) return false
      await this.openPort(ports[0])
      return true
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
      const info = parseInfo((await this.request(Cmd.GetInfo)).payload)
      this.events.info?.(info)
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
      this.frameCount++
      this.byteCount += fp.jpeg.length
      this.events.frame(fp)
      return
    }
    const p = this.pending.get(f.seq)
    if (!p) return // late/unknown response
    if (f.cmd === Cmd.Error) {
      const e = parseError(f.payload)
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
    await this.request(Cmd.StreamStart, encodeStreamStart({ frames: true, results: false, maxFps, jpegQuality: 0 }))
    this.stopTimers()
    this.frameCount = 0
    this.byteCount = 0
    // Firmware stops streaming after 3 s without a request (protocol.md §6).
    this.pingTimer = setInterval(() => {
      this.request(Cmd.Ping).catch(() => {})
    }, PING_INTERVAL_MS)
    this.statsTimer = setInterval(() => {
      const secs = STATS_INTERVAL_MS / 1000
      this.events.stats({ fps: this.frameCount / secs, kbps: this.byteCount / 1024 / secs })
      this.frameCount = 0
      this.byteCount = 0
    }, STATS_INTERVAL_MS)
  }

  async stopStream(): Promise<void> {
    this.stopTimers()
    this.events.stats({ fps: 0, kbps: 0 })
    await this.request(Cmd.StreamStop)
  }

  private stopTimers() {
    if (this.pingTimer) clearInterval(this.pingTimer)
    if (this.statsTimer) clearInterval(this.statsTimer)
    this.pingTimer = null
    this.statsTimer = null
  }

  async disconnect(): Promise<void> {
    const s = this.session
    if (s) await this.teardown(s, new Error('disconnected by user'))
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
      await s.writer.close().catch(() => {})
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
