import { act, renderHook, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { Decoder, encodeFrame } from '../protocol/codec'
import { Cmd, responseOf } from '../protocol/messages'
import { DemoDevice } from './DemoDevice'
import { RealDevice } from './RealDevice'
import { NotSupportedError, type Device, type FrameSource } from './types'
import { useDevice } from './useDevice'

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
})
afterEach(() => {
  vi.useRealTimers()
})

const idleSerial = {
  addEventListener: () => {},
  removeEventListener: () => {},
  getPorts: async () => [],
  requestPort: async () => {
    throw new DOMException('cancel', 'NotFoundError')
  },
} as unknown as Serial

function serialWithInfo(info: Uint8Array): Serial {
  let ctl!: ReadableStreamDefaultController<Uint8Array>
  const dec = new Decoder()
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    open: async () => {},
    close: async () => {},
    readable: new ReadableStream<Uint8Array>({
      start: (c) => {
        ctl = c
      },
    }),
    writable: new WritableStream<Uint8Array>({
      write: (chunk) => {
        for (const f of dec.push(chunk)) {
          ctl.enqueue(encodeFrame(responseOf(f.cmd), f.seq, f.cmd === Cmd.GetInfo ? info : new Uint8Array(0)))
        }
      },
    }),
  }
  return { ...idleSerial, getPorts: async () => [port] } as unknown as Serial
}

test('DemoDevice starts connected with all three functions and DEMO firmware', () => {
  const d = new DemoDevice()
  const s = d.snapshot()
  expect(s.mode).toBe('demo')
  expect(s.status).toBe('connected')
  expect(s.supported).toEqual([1, 2, 3])
  expect(s.info?.firmwareVersion).toBe('DEMO')
  expect(s.dirty).toBe(false)
  d.dispose()
})

test('DemoDevice emits canvas frames every 100 ms and stops after dispose', () => {
  vi.useFakeTimers()
  const d = new DemoDevice()
  const frames: FrameSource[] = []
  d.onFrame((f) => frames.push(f))
  vi.advanceTimersByTime(350)
  expect(frames).toHaveLength(3)
  expect(frames.every((f) => f.kind === 'canvas')).toBe(true)
  d.dispose()
  vi.advanceTimersByTime(1000)
  expect(frames).toHaveLength(3)
})

test('DemoDevice dirty flag: edits set it, save and factoryReset clear it', async () => {
  const d = new DemoDevice()
  const notified = vi.fn()
  d.subscribe(notified)
  await d.setParams({ color: { tolerance: 50, minArea: 100, maxResults: 4, showNames: true } })
  expect(d.snapshot().dirty).toBe(true)
  expect(d.snapshot().params.color.tolerance).toBe(50)
  expect(notified).toHaveBeenCalled()
  await d.save()
  expect(d.snapshot().dirty).toBe(false)
  const id = await d.learnColor({ x: 0, y: 0, width: 10, height: 10 })
  await d.setName(1, id, 'red')
  expect(d.snapshot().learned[1][0].name).toBe('red')
  expect(d.snapshot().dirty).toBe(true)
  await d.setSystem({ ledBrightness: 10 })
  await d.factoryReset()
  const s = d.snapshot()
  expect(s.dirty).toBe(false)
  expect(s.learned[1]).toEqual([])
  expect(s.system).toEqual({ uartBaud: 115200, i2cAddress: 0x5a, ledBrightness: 50 })
  expect(s.params.color.tolerance).toBe(30)
  d.dispose()
})

test('DemoDevice export/import round-trips; malformed input is rejected and leaves state unchanged', async () => {
  const a = new DemoDevice()
  const id = await a.learnColor({ x: 0, y: 0, width: 4, height: 4 })
  await a.setName(1, id, 'x')
  const json = a.exportConfig()
  expect(JSON.parse(json).version).toBe(1)
  const b = new DemoDevice()
  await b.importConfig(json)
  expect(b.snapshot().learned).toEqual(a.snapshot().learned)
  const before = b.exportConfig()
  for (const bad of ['nope', '{}', '{"version":2}', JSON.stringify({ version: 1, learned: {}, params: {}, system: {} })]) {
    await expect(b.importConfig(bad)).rejects.toThrow(Error)
  }
  expect(b.exportConfig()).toBe(before)
  a.dispose()
  b.dispose()
})

test('RealDevice with empty functionIds supports nothing and rejects mutations', async () => {
  const d: Device = new RealDevice(serialWithInfo(new Uint8Array(23)))
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  expect(d.snapshot().mode).toBe('real')
  expect(d.snapshot().supported).toEqual([])
  await expect(d.learnColor({ x: 0, y: 0, width: 1, height: 1 })).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.setParams({})).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.save()).rejects.toBeInstanceOf(NotSupportedError)
  expect(JSON.parse(d.exportConfig()).version).toBe(1)
  d.dispose()
})

test('RealDevice reports supported functions from GET_INFO, ignoring unknown ids', async () => {
  const info = new Uint8Array(23 + 3)
  info[22] = 3
  info.set([1, 3, 9], 23)
  const d = new RealDevice(serialWithInfo(info))
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  expect(d.snapshot().supported).toEqual([1, 3])
  d.dispose()
})

test('RealDevice without Web Serial is unsupported', () => {
  const d = new RealDevice(undefined)
  expect(d.snapshot().status).toBe('unsupported')
  d.dispose()
})

test('useDevice: demo toggle persists, disposes the old device, and demo -> real -> demo leaves no old data', async () => {
  const { result } = renderHook(() => useDevice(idleSerial))
  await waitFor(() => expect(result.current.device).not.toBeNull())
  expect(result.current.snapshot.mode).toBe('real')
  expect(result.current.snapshot.status).toBe('disconnected')

  act(() => result.current.setDemo(true))
  await waitFor(() => expect(result.current.snapshot.mode).toBe('demo'))
  expect(localStorage.getItem('mxcam.demo')).toBe('1')
  const first = result.current.device!
  const spy = vi.spyOn(first, 'dispose')
  await act(async () => {
    await first.learnColor({ x: 0, y: 0, width: 5, height: 5 })
  })
  expect(result.current.snapshot.learned[1]).toHaveLength(1)
  expect(result.current.snapshot.dirty).toBe(true)

  act(() => result.current.setDemo(false))
  await waitFor(() => expect(result.current.snapshot.mode).toBe('real'))
  expect(spy).toHaveBeenCalled()
  expect(result.current.snapshot.status).toBe('disconnected')
  expect(result.current.snapshot.learned[1]).toEqual([])

  act(() => result.current.setDemo(true))
  await waitFor(() => expect(result.current.snapshot.mode).toBe('demo'))
  const second = result.current.device!
  const results = vi.fn()
  second.onResults(results)
  expect(second).not.toBe(first)
  expect(result.current.snapshot.learned).toEqual({ 1: [], 2: [], 3: [] })
  expect(result.current.snapshot.dirty).toBe(false)
  expect(result.current.snapshot.status).toBe('connected')
  expect(results).not.toHaveBeenCalled()
})

test('useDevice restores the saved demo mode', async () => {
  localStorage.setItem('mxcam.demo', '1')
  const { result } = renderHook(() => useDevice(idleSerial))
  await waitFor(() => expect(result.current.snapshot.mode).toBe('demo'))
})
