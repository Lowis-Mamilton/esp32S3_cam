import { afterEach, describe, expect, test, vi } from 'vitest'
import { Decoder, encodeFrame, type Frame } from '../protocol/codec'
import { Cmd, responseOf } from '../protocol/messages'
import { MxcamConnection, type ConnectionStatus } from './connection'

function infoPayload(): Uint8Array {
  const p = new Uint8Array(24)
  p[0] = 1
  p[1] = 0
  p[2] = 1
  p[3] = 0
  p.set(new TextEncoder().encode('abc1234'), 4)
  p.set([1, 2, 3, 4, 5, 6], 11)
  new DataView(p.buffer).setUint16(17, 320, true)
  new DataView(p.buffer).setUint16(19, 240, true)
  p[21] = 8
  p[22] = 1
  p[23] = 1
  return p
}

function framePushPayload(id: number, jpeg: Uint8Array): Uint8Array {
  const p = new Uint8Array(12 + jpeg.length)
  const v = new DataView(p.buffer)
  v.setUint32(0, id, true)
  v.setUint32(4, 1000 + id, true)
  v.setUint16(8, 320, true)
  v.setUint16(10, 240, true)
  p.set(jpeg, 12)
  return p
}

type Handler = (f: Frame) => Uint8Array | null | { cmd: number; payload: Uint8Array }

class FakePort {
  requests: Frame[] = []
  opened = false
  closed = false
  openOptions: SerialOptions | undefined
  openCount = 0
  log: string[] = []
  async setSignals(o: SerialOutputSignals) {
    this.log.push('signals:' + JSON.stringify(o))
  }
  vid = 0x303a
  pid = 0x1001
  answering = true
  getInfo() {
    return { usbVendorId: this.vid, usbProductId: this.pid }
  }
  handler: Handler
  private controller!: ReadableStreamDefaultController<Uint8Array>
  readable: ReadableStream<Uint8Array>
  writable: WritableStream<Uint8Array>

  constructor(handler?: Handler) {
    this.handler =
      handler ??
      ((f) => {
        if (f.cmd === Cmd.GetInfo) return infoPayload()
        return new Uint8Array(0)
      })
    this.readable = this.makeReadable()
    this.writable = this.makeWritable()
  }
  private makeReadable() {
    return new ReadableStream<Uint8Array>({
      start: (c) => {
        this.controller = c
      },
    })
  }
  private makeWritable() {
    const dec = new Decoder()
    return new WritableStream<Uint8Array>({
      write: (chunk) => {
        for (const f of dec.push(chunk)) {
          this.requests.push(f)
          if (this.requests.length === 1) this.log.push('write')
          if (!this.answering && f.cmd === Cmd.Ping) continue
          const r = this.handler(f)
          if (r === null) continue
          if ('cmd' in r) this.push(encodeFrame(r.cmd, f.seq, r.payload))
          else this.push(encodeFrame(responseOf(f.cmd), f.seq, r))
        }
      },
    })
  }
  async open(o: SerialOptions) {
    if (this.opened) {
      // a real port hands out fresh streams after being reopened
      this.readable = this.makeReadable()
      this.writable = this.makeWritable()
    }
    this.opened = true
    this.openCount++
    this.openOptions = o
  }
  async close() {
    this.closed = true
  }
  push(bytes: Uint8Array) {
    this.controller.enqueue(bytes)
  }
  endReadable() {
    this.controller.close()
  }
}

class FakeSerial {
  listeners = new Map<string, Array<(e: unknown) => void>>()
  granted: FakePort[] = []
  nextPort: FakePort | 'cancel'
  constructor(port: FakePort | 'cancel') {
    this.nextPort = port
  }
  addEventListener(type: string, l: (e: unknown) => void) {
    this.listeners.set(type, [...(this.listeners.get(type) ?? []), l])
  }
  removeEventListener(type: string, l: (e: unknown) => void) {
    this.listeners.set(type, (this.listeners.get(type) ?? []).filter((x) => x !== l))
  }
  emit(type: string, port: FakePort) {
    for (const l of this.listeners.get(type) ?? []) l({ type, target: port })
  }
  requestOptions: SerialPortRequestOptions | undefined
  async requestPort(o?: SerialPortRequestOptions) {
    this.requestOptions = o
    if (this.nextPort === 'cancel') throw new DOMException('no port', 'NotFoundError')
    this.granted.push(this.nextPort)
    return this.nextPort
  }
  async getPorts() {
    return this.granted
  }
}

function setup(handler?: Handler) {
  const port = new FakePort(handler)
  const serial = new FakeSerial(port)
  const statuses: ConnectionStatus[] = []
  const frames: number[] = []
  const conn = new MxcamConnection(serial as unknown as Serial, {
    status: (s) => statuses.push(s),
    frame: (f) => frames.push(f.frameId),
    stats: () => {},
  })
  return { port, serial, statuses, frames, conn }
}

// Unhandled rejections are not hooked here: vitest fails the run on any unhandled error.
afterEach(() => {
  vi.useRealTimers()
})

const silentOn = (cmd: number): Handler => (f) =>
  f.cmd === Cmd.GetInfo ? infoPayload() : f.cmd === cmd ? null : new Uint8Array(0)

const noInfo: Handler = (f) => (f.cmd === Cmd.GetInfo ? null : new Uint8Array(0))

describe('MxcamConnection', () => {
  test('serial undefined reports unsupported', () => {
    const statuses: ConnectionStatus[] = []
    new MxcamConnection(undefined, { status: (s) => statuses.push(s), frame: () => {}, stats: () => {} })
    expect(statuses).toEqual(['unsupported'])
  })

  test('connect opens at 115200, runs GET_INFO then STREAM_START', async () => {
    const { conn, port, statuses } = setup()
    await conn.connect()
    expect(port.openOptions?.baudRate).toBe(115200)
    expect(port.requests.map((r) => r.cmd)).toEqual([Cmd.GetInfo, Cmd.StreamStart])
    expect([...port.requests[1].payload]).toEqual([1, 10, 0])
    expect(statuses).toEqual(['connecting', 'connected'])
    await conn.disconnect()
    expect(statuses.at(-1)).toBe('disconnected')
    expect(port.closed).toBe(true)
  })

  test('cancelled port picker leaves the connection disconnected', async () => {
    const serial = new FakeSerial('cancel')
    const statuses: ConnectionStatus[] = []
    const conn = new MxcamConnection(serial as unknown as Serial, {
      status: (s) => statuses.push(s),
      frame: () => {},
      stats: () => {},
    })
    await conn.connect()
    expect(statuses.at(-1)).not.toBe('connected')
    expect(statuses.at(-1)).not.toBe('connecting')
  })

  test('pairs responses by seq even when answered out of order', async () => {
    const held: Frame[] = []
    const { conn, port } = setup((f) => {
      if (f.cmd === Cmd.GetInfo) return infoPayload()
      if (f.cmd === Cmd.GetFunction) {
        held.push(f)
        return null
      }
      return new Uint8Array(0)
    })
    await conn.connect()
    const a = conn.request(Cmd.GetFunction, new Uint8Array([1]))
    const b = conn.request(Cmd.GetFunction, new Uint8Array([2]))
    await vi.waitFor(() => expect(held.length).toBe(2))
    port.push(encodeFrame(responseOf(Cmd.GetFunction), held[1].seq, new Uint8Array([22])))
    port.push(encodeFrame(responseOf(Cmd.GetFunction), held[0].seq, new Uint8Array([11])))
    expect([...(await a).payload]).toEqual([11])
    expect([...(await b).payload]).toEqual([22])
    await conn.disconnect()
  })

  test('a stale ERROR for another command with the same seq does not fail the pending request', async () => {
    const held: Frame[] = []
    const { conn, port } = setup((f) => {
      if (f.cmd === Cmd.GetInfo) return infoPayload()
      if (f.cmd === Cmd.GetFunction) {
        held.push(f)
        return null
      }
      return new Uint8Array(0)
    })
    await conn.connect()
    const req = conn.request(Cmd.GetFunction, new Uint8Array(0))
    await vi.waitFor(() => expect(held.length).toBe(1))
    // e.g. a LEARN-cancel ERROR that was still in the USB ring
    port.push(encodeFrame(Cmd.Error, held[0].seq, new Uint8Array([Cmd.Learn, 3])))
    port.push(encodeFrame(responseOf(Cmd.GetFunction), held[0].seq, new Uint8Array([2])))
    expect([...(await req).payload]).toEqual([2])
    await conn.disconnect()
  })

  test('request times out and rejects', async () => {
    const { conn } = setup(silentOn(Cmd.GetFunction))
    await conn.connect()
    await expect(conn.request(Cmd.GetFunction, new Uint8Array(0), 30)).rejects.toThrow(/timeout/i)
    await conn.disconnect()
  })

  test('ERROR response rejects with the error content', async () => {
    const { conn } = setup((f) => {
      if (f.cmd === Cmd.GetInfo) return infoPayload()
      if (f.cmd === Cmd.SetFunction) return { cmd: Cmd.Error, payload: new Uint8Array([Cmd.SetFunction, 3]) }
      return new Uint8Array(0)
    })
    await conn.connect()
    await expect(conn.request(Cmd.SetFunction, new Uint8Array([9]))).rejects.toMatchObject({
      cmd: Cmd.SetFunction,
      code: 3,
    })
    await conn.disconnect()
  })

  test('review point 3: read end rejects all pending, status disconnected, no unhandled rejection', async () => {
    const { conn, port, statuses } = setup(silentOn(Cmd.GetFunction))
    await conn.connect()
    const a = conn.request(Cmd.GetFunction, new Uint8Array([1]), 5000)
    const b = conn.request(Cmd.GetFunction, new Uint8Array([2]), 5000)
    const ra = a.then(() => null, (e: unknown) => e)
    const rb = b.then(() => null, (e: unknown) => e)
    await vi.waitFor(() => expect(port.requests.filter((r) => r.cmd === Cmd.GetFunction).length).toBe(2))
    port.endReadable()
    expect(await ra).toBeInstanceOf(Error)
    expect(await rb).toBeInstanceOf(Error)
    expect(statuses.at(-1)).toBe('disconnected')
    await new Promise((r) => setTimeout(r, 20))
    await expect(conn.request(Cmd.Ping)).rejects.toThrow()
  })

  test('serial disconnect event for our port tears down', async () => {
    const { conn, port, serial, statuses } = setup()
    await conn.connect()
    serial.emit('disconnect', port)
    await vi.waitFor(() => expect(statuses.at(-1)).toBe('disconnected'))
    await new Promise((r) => setTimeout(r, 20))
  })

  test('review point 4: 300 sequential requests each get their own seq response', async () => {
    const { conn } = setup((f) => (f.cmd === Cmd.GetInfo ? infoPayload() : new Uint8Array([f.seq])))
    await conn.connect()
    for (let i = 0; i < 300; i++) {
      const r = await conn.request(Cmd.GetFunction)
      expect(r.payload[0]).toBe(r.seq)
    }
    await conn.disconnect()
  })

  test('300 requests in concurrent batches of 50 do not mismatch', async () => {
    const { conn } = setup((f) => (f.cmd === Cmd.GetInfo ? infoPayload() : new Uint8Array([f.seq])))
    await conn.connect()
    for (let b = 0; b < 6; b++) {
      const rs = await Promise.all(Array.from({ length: 50 }, () => conn.request(Cmd.GetFunction)))
      for (const r of rs) expect(r.payload[0]).toBe(r.seq)
    }
    await conn.disconnect()
  })

  test('FRAME push fires the frame event', async () => {
    const { conn, port, frames } = setup()
    await conn.connect()
    port.push(encodeFrame(Cmd.PushFrame, 0, framePushPayload(7, new Uint8Array([0xff, 0xd8, 0xff, 0xd9]))))
    await vi.waitFor(() => expect(frames).toEqual([7]))
    await conn.disconnect()
  })

  test('pings every second while streaming and stops after disconnect', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { conn, port } = setup()
    await conn.connect()
    const pings = () => port.requests.filter((r) => r.cmd === Cmd.Ping).length
    expect(pings()).toBe(0)
    await vi.advanceTimersByTimeAsync(1000)
    expect(pings()).toBe(1)
    await vi.advanceTimersByTimeAsync(2000)
    expect(pings()).toBe(3)
    await conn.disconnect()
    await vi.advanceTimersByTimeAsync(3000)
    expect(pings()).toBe(3)
  })

  test('autoConnect reuses a granted port; serial connect event triggers it', async () => {
    const { conn, port, serial, statuses } = setup()
    serial.granted.push(port)
    expect(await conn.autoConnect()).toBe(true)
    expect(statuses.at(-1)).toBe('connected')
    await conn.disconnect()
    serial.emit('connect', port)
    await vi.waitFor(() => expect(statuses.at(-1)).toBe('connected'))
    await conn.disconnect()
  })

  test('overlapping autoConnect calls open the port once and end connected', async () => {
    const { conn, port, serial, statuses } = setup()
    serial.granted.push(port)
    const [a, b] = await Promise.all([conn.autoConnect(), conn.autoConnect()])
    expect([a, b].filter(Boolean).length).toBe(1)
    expect(port.openCount).toBe(1)
    expect(statuses.at(-1)).toBe('connected')
    await conn.disconnect()
  })

  test('autoConnect + connect event for the same port does not break the live link', async () => {
    const { conn, port, serial, statuses } = setup()
    serial.granted.push(port)
    const p = conn.autoConnect()
    serial.emit('connect', port)
    await p
    await new Promise((r) => setTimeout(r, 20))
    expect(port.openCount).toBe(1)
    expect(statuses.at(-1)).toBe('connected')
    await conn.disconnect()
  })

  test('three consecutive PING failures tear the link down; success resets the count', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { conn, port, statuses } = setup()
    await conn.connect()
    port.answering = false
    await vi.advanceTimersByTimeAsync(2500) // one ping has failed (t=2 s)
    port.answering = true
    await vi.advanceTimersByTimeAsync(3000) // the ping from t=2 s fails too, then successes reset the count
    port.answering = false
    await vi.advanceTimersByTimeAsync(2000) // only one failure since the reset
    expect(statuses.at(-1)).toBe('connected')
    await vi.advanceTimersByTimeAsync(4000) // three in a row
    expect(statuses.at(-1)).toBe('disconnected')
  })

  test('requestPort and autoConnect use the MXCam VID/PID', async () => {
    const other = new FakePort()
    other.vid = 0x1234
    other.pid = 0x5678
    const mx = new FakePort()
    const serial = new FakeSerial(mx)
    serial.granted.push(other, mx)
    const conn = new MxcamConnection(serial as unknown as Serial, { status: () => {}, frame: () => {}, stats: () => {} })
    expect(await conn.autoConnect()).toBe(true)
    expect(other.openCount).toBe(0)
    expect(mx.openCount).toBe(1)
    await conn.disconnect()
    await conn.connect()
    expect(serial.requestOptions?.filters).toEqual([{ usbVendorId: 0x303a, usbProductId: 0x1001 }])
    await conn.disconnect()
  })

  test('autoConnect returns false when no granted port matches', async () => {
    const other = new FakePort()
    other.vid = 0x1234
    const serial = new FakeSerial(other)
    serial.granted.push(other)
    const conn = new MxcamConnection(serial as unknown as Serial, { status: () => {}, frame: () => {}, stats: () => {} })
    expect(await conn.autoConnect()).toBe(false)
    expect(other.openCount).toBe(0)
  })

  test('drops RTS then DTR right after open, before the first write', async () => {
    const { conn, port } = setup()
    await conn.connect()
    expect(port.log.slice(0, 3)).toEqual([
      'signals:{"requestToSend":false}',
      'signals:{"dataTerminalReady":false}',
      'write',
    ])
    await conn.disconnect()
  })

  test('setSignals failure does not fail the connection', async () => {
    const { conn, port, statuses } = setup()
    port.setSignals = async () => {
      throw new Error('unsupported')
    }
    await conn.connect()
    expect(statuses.at(-1)).toBe('connected')
    await conn.disconnect()
  })

  test('watchdog: re-sends STREAM_START when frames stop for 2.5 s', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { conn, port } = setup()
    await conn.connect()
    const starts = () => port.requests.filter((r) => r.cmd === Cmd.StreamStart).length
    expect(starts()).toBe(1)
    await vi.advanceTimersByTimeAsync(2000)
    expect(starts()).toBe(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(starts()).toBe(2)
    expect([...port.requests.filter((r) => r.cmd === Cmd.StreamStart)[1].payload]).toEqual([1, 10, 0])
    await conn.disconnect()
  })

  test('watchdog: no re-send while frames keep arriving', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { conn, port } = setup()
    await conn.connect()
    for (let i = 0; i < 10; i++) {
      port.push(encodeFrame(Cmd.PushFrame, 0, framePushPayload(i, new Uint8Array([0xff, 0xd8, 0xff, 0xd9]))))
      await vi.advanceTimersByTimeAsync(1000)
    }
    expect(port.requests.filter((r) => r.cmd === Cmd.StreamStart).length).toBe(1)
    await conn.disconnect()
  })

  test('cold boot: GET_INFO is retried until the device answers', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    let ready = false
    const { conn, port, serial, statuses } = setup((f) => (f.cmd === Cmd.GetInfo ? (ready ? infoPayload() : null) : new Uint8Array(0)))
    serial.granted.push(port)
    setTimeout(() => {
      ready = true
    }, 2500)
    const p = conn.autoConnect()
    await vi.advanceTimersByTimeAsync(4000)
    expect(await p).toBe(true)
    expect(statuses.at(-1)).toBe('connected')
    await conn.disconnect()
  })

  test('cold boot: gives up after about 5 s when the device never answers', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { conn, port, serial, statuses } = setup(noInfo)
    serial.granted.push(port)
    const p = conn.autoConnect()
    await vi.advanceTimersByTimeAsync(6000)
    expect(await p).toBe(false)
    expect(statuses.at(-1)).toBe('disconnected')
    expect(port.requests.filter((r) => r.cmd === Cmd.GetInfo).length).toBeGreaterThanOrEqual(3)
  })

  test('disconnect sends a best-effort STREAM_STOP, even if it is never answered', async () => {
    const { conn, port } = setup(silentOn(Cmd.StreamStop))
    await conn.connect()
    await conn.disconnect()
    expect(port.requests.at(-1)?.cmd).toBe(Cmd.StreamStop)
    expect(port.closed).toBe(true)
  })

  test('autoConnect tries the next matching port when the first never answers', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const dead = new FakePort(noInfo)
    const good = new FakePort()
    const serial = new FakeSerial(good)
    serial.granted.push(dead, good)
    const conn = new MxcamConnection(serial as unknown as Serial, { status: () => {}, frame: () => {}, stats: () => {} })
    const p = conn.autoConnect()
    await vi.advanceTimersByTimeAsync(6000)
    expect(await p).toBe(true)
    expect(good.openCount).toBe(1)
    await conn.disconnect()
  })
})

// ---- M2: results pushes, stream flags and the setup hook ----
function resultsPayload(frameId: number, fn: number): Uint8Array {
  const p = new Uint8Array(6 + 12)
  const v = new DataView(p.buffer)
  v.setUint32(0, frameId, true)
  p[4] = fn
  p[5] = 1
  v.setUint16(6, 2, true)
  v.setInt16(8, 160, true)
  v.setInt16(10, 120, true)
  v.setUint16(12, 30, true)
  v.setUint16(14, 20, true)
  p[16] = 255
  return p
}

describe('MxcamConnection M2 additions', () => {

  test('streamResults sets STREAM_START flags bit1; RESULTS pushes reach the results event', async () => {
    const port = new FakePort()
    const serial = new FakeSerial(port)
    const got: { frameId: number; functionId: number; n: number }[] = []
    const conn = new MxcamConnection(
      serial as unknown as Serial,
      {
        status: () => {},
        frame: () => {},
        stats: () => {},
        results: (r) => got.push({ frameId: r.frameId, functionId: r.functionId, n: r.results.length }),
      },
      { streamResults: true },
    )
    await conn.connect()
    expect([...port.requests.find((r) => r.cmd === Cmd.StreamStart)!.payload]).toEqual([3, 10, 0])
    port.push(encodeFrame(Cmd.PushResults, 0, resultsPayload(42, 1)))
    await vi.waitFor(() => expect(got).toEqual([{ frameId: 42, functionId: 1, n: 1 }]))
    await conn.disconnect()
  })

  test('a RESULTS push without a results handler is ignored; default flags stay frames-only', async () => {
    const { conn, port, frames } = setup()
    await conn.connect()
    port.push(encodeFrame(Cmd.PushResults, 0, resultsPayload(1, 1)))
    port.push(encodeFrame(Cmd.PushFrame, 1, framePushPayload(2, new Uint8Array([1]))))
    await vi.waitFor(() => expect(frames).toEqual([2]))
    expect([...port.requests.find((r) => r.cmd === Cmd.StreamStart)!.payload]).toEqual([1, 10, 0])
    await conn.disconnect()
  })

  test('setup hook runs after GET_INFO and before STREAM_START; status stays connecting meanwhile', async () => {
    const port = new FakePort()
    const serial = new FakeSerial(port)
    const statuses: ConnectionStatus[] = []
    const conn: MxcamConnection = new MxcamConnection(serial as unknown as Serial, {
      status: (s) => statuses.push(s),
      frame: () => {},
      stats: () => {},
      setup: async (info) => {
        expect(info.functionIds).toEqual([1])
        expect(statuses.at(-1)).toBe('connecting')
        await conn.request(Cmd.GetConfig)
      },
    })
    await conn.connect()
    expect(port.requests.map((r) => r.cmd)).toEqual([Cmd.GetInfo, Cmd.GetConfig, Cmd.StreamStart])
    expect(statuses).toEqual(['connecting', 'connected'])
    await conn.disconnect()
  })

  test('a failing setup hook fails the connect and closes the port', async () => {
    const port = new FakePort()
    const serial = new FakeSerial(port)
    const conn = new MxcamConnection(serial as unknown as Serial, {
      status: () => {},
      frame: () => {},
      stats: () => {},
      setup: async () => {
        throw new Error('boom')
      },
    })
    await expect(conn.connect()).rejects.toThrow('boom')
    expect(port.closed).toBe(true)
    expect(port.requests.some((r) => r.cmd === Cmd.StreamStart)).toBe(false)
  })
})

describe('paused stream (results only)', () => {
  test('watchdog / visibility re-send after pausing still carries flags 0x02', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const port = new FakePort()
    const conn = new MxcamConnection(
      new FakeSerial(port) as unknown as Serial,
      { status: () => {}, frame: () => {}, stats: () => {} },
      { streamResults: true },
    )
    await conn.connect()
    await conn.setFrames(false)
    const starts = () => port.requests.filter((r) => r.cmd === Cmd.StreamStart)
    const n = starts().length
    await vi.advanceTimersByTimeAsync(3000)
    expect(starts().length).toBe(n + 1)
    expect([...starts().at(-1)!.payload]).toEqual([2, 10, 0])
    await conn.disconnect()
  })

  test('RESULTS pushes keep the watchdog quiet when results streaming is on', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const port = new FakePort()
    const conn = new MxcamConnection(
      new FakeSerial(port) as unknown as Serial,
      { status: () => {}, frame: () => {}, stats: () => {}, results: () => {} },
      { streamResults: true },
    )
    await conn.connect()
    await conn.setFrames(false)
    const n = port.requests.filter((r) => r.cmd === Cmd.StreamStart).length
    for (let i = 0; i < 10; i++) {
      port.push(encodeFrame(Cmd.PushResults, 0, resultsPayload(i, 1)))
      await vi.advanceTimersByTimeAsync(1000)
    }
    expect(port.requests.filter((r) => r.cmd === Cmd.StreamStart).length).toBe(n)
    await conn.disconnect()
  })
})
