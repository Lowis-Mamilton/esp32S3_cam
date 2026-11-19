import { act, renderHook, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { Decoder, encodeFrame } from '../protocol/codec'
import { Cmd, ErrorCode, responseOf } from '../protocol/messages'
import { DemoDevice } from './DemoDevice'
import { RealDevice } from './RealDevice'
import { sceneAt } from './demo/scene'
import { LearnError, NotSupportedError, emptyLearned, toFunctionIds, type Device, type FrameSource, type Result } from './types'
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

const M1_CMDS: number[] = [Cmd.Ping, Cmd.SetFunction, Cmd.GetFunction, Cmd.StreamStart, Cmd.StreamStop]

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
          // M1 firmware: only the M1 commands; everything else is UNKNOWN_CMD
          if (f.cmd === Cmd.GetInfo) ctl.enqueue(encodeFrame(responseOf(f.cmd), f.seq, info))
          else if (M1_CMDS.includes(f.cmd)) ctl.enqueue(encodeFrame(responseOf(f.cmd), f.seq, new Uint8Array(0)))
          else ctl.enqueue(encodeFrame(Cmd.Error, f.seq, new Uint8Array([f.cmd, ErrorCode.UnknownCmd])))
        }
      },
    }),
  }
  return { ...idleSerial, getPorts: async () => [port] } as unknown as Serial
}

test('DemoDevice starts connected with all three functions and DEMO firmware', () => {
  const d = new DemoDevice(() => 0)
  const s = d.snapshot()
  expect(s.mode).toBe('demo')
  expect(s.status).toBe('connected')
  expect(s.supported).toEqual([1, 2, 3, 8])
  expect(s.info?.firmwareVersion).toBe('DEMO')
  expect(s.dirty).toBe(false)
  d.dispose()
})

const FULL = { x: 0, y: 0, width: 320, height: 240 }

test('DemoDevice emits canvas frames every 100 ms and stops after dispose', () => {
  vi.useFakeTimers()
  const d = new DemoDevice(() => 0)
  const frames: FrameSource[] = []
  d.onFrame((f) => frames.push(f))
  vi.advanceTimersByTime(350)
  expect(frames).toHaveLength(3)
  expect(frames.every((f) => f.kind === 'canvas')).toBe(true)
  d.dispose()
  vi.advanceTimersByTime(1000)
  expect(frames).toHaveLength(3)
})

test('DemoDevice: setting params or system to the current value does not make the config dirty', async () => {
  const d = new DemoDevice(() => 0)
  const notified = vi.fn()
  d.subscribe(notified)
  await d.setParams({ color: { ...d.snapshot().params.color } })
  await d.setParams({})
  await d.setSystem({ ledBrightness: d.snapshot().system.ledBrightness })
  await d.setSystem({})
  expect(d.snapshot().dirty).toBe(false)
  expect(notified).not.toHaveBeenCalled()
  await d.setSystem({ ledBrightness: 10 })
  expect(d.snapshot().dirty).toBe(true)
  d.dispose()
})

test('DemoDevice dirty flag: edits set it, save and factoryReset clear it', async () => {
  const d = new DemoDevice(() => 0)
  const notified = vi.fn()
  d.subscribe(notified)
  await d.setParams({ color: { tolerance: 50, minArea: 100, maxResults: 4, showNames: true } })
  expect(d.snapshot().dirty).toBe(true)
  expect(d.snapshot().params.color.tolerance).toBe(50)
  expect(notified).toHaveBeenCalled()
  await d.save()
  expect(d.snapshot().dirty).toBe(false)
  const id = await d.learnColor(FULL)
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
  const a = new DemoDevice(() => 0)
  const id = await a.learnColor(FULL)
  await a.setName(1, id, 'x')
  const json = a.exportConfig()
  expect(JSON.parse(json).version).toBe(1)
  const b = new DemoDevice(() => 0)
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
  expect(() => d.exportConfig()).toThrow(NotSupportedError)
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
  vi.useFakeTimers()
  const { result } = renderHook(() => useDevice(idleSerial))
  expect(result.current.device).not.toBeNull()
  expect(result.current.snapshot.mode).toBe('real')
  expect(result.current.snapshot.status).toBe('disconnected')

  act(() => result.current.setDemo(true))
  expect(result.current.snapshot.mode).toBe('demo')
  expect(localStorage.getItem('mxcam.demo')).toBe('1')
  const first = result.current.device!
  const spy = vi.spyOn(first, 'dispose')
  await act(async () => {
    await first.learnColor(FULL)
  })
  expect(result.current.snapshot.learned[1]).toHaveLength(1)
  expect(result.current.snapshot.dirty).toBe(true)

  act(() => result.current.setDemo(false))
  expect(result.current.snapshot.mode).toBe('real')
  expect(spy).toHaveBeenCalled()
  expect(result.current.snapshot.status).toBe('disconnected')
  expect(result.current.snapshot.learned[1]).toEqual([])

  act(() => result.current.setDemo(true))
  const second = result.current.device!
  expect(second).not.toBe(first)
  const batches: unknown[][] = []
  second.onResults((r) => batches.push(r))
  act(() => {
    vi.advanceTimersByTime(300)
  })
  expect(batches.length).toBeGreaterThanOrEqual(2)
  expect(batches.some((b) => b.length > 0)).toBe(true)
  expect(batches.every((b) => (b as { kind: string }[]).every((r) => r.kind === 'color'))).toBe(true)
  expect(result.current.snapshot.learned).toEqual({ 1: [], 2: [], 3: [], 8: [] })
  expect(result.current.snapshot.dirty).toBe(false)
  expect(result.current.snapshot.status).toBe('connected')
})

test('failed import leaves learned, params, system and dirty exactly unchanged', async () => {
  const d = new DemoDevice(() => 0)
  await d.learnColor(FULL)
  await d.save()
  const before = d.snapshot()
  const beforeJson = d.exportConfig()
  const good = JSON.parse(beforeJson)
  const newLearned = { ...good.learned, 1: [{ id: 2, name: 'new', color: [1, 2, 3] }] }
  const cases = [
    { ...good, learned: newLearned, params: { ...good.params, color: { ...good.params.color, tolerance: 500 } } },
    { ...good, learned: newLearned, system: { ...good.system, i2cAddress: 0x01 } },
    { ...good, learned: newLearned, params: null },
    { ...good, learned: newLearned, system: 'x' },
  ]
  for (const c of cases) {
    await expect(d.importConfig(JSON.stringify(c))).rejects.toThrow(Error)
    const s = d.snapshot()
    expect(s.learned).toEqual(before.learned)
    expect(s.params).toEqual(before.params)
    expect(s.system).toEqual(before.system)
    expect(s.dirty).toBe(false)
    expect(d.exportConfig()).toBe(beforeJson)
  }
  d.dispose()
})

test('useDevice restores the saved demo mode', async () => {
  localStorage.setItem('mxcam.demo', '1')
  const { result } = renderHook(() => useDevice(idleSerial))
  await waitFor(() => expect(result.current.snapshot.mode).toBe('demo'))
})

// ---- Task 3: DemoDevice learning on the demo scene ----
test('DemoDevice.learnColor: lowest free id, same colour updates, background rejects, 8th rejects', async () => {
  const d = new DemoDevice(() => 0)
  await expect(d.learnColor({ x: 0, y: 0, width: 2, height: 2 })).rejects.toMatchObject({ code: 'no-color' })
  const id1 = await d.learnColor(FULL)
  expect(id1).toBe(1)
  expect(d.snapshot().learned[1][0].name).toBe('')
  expect(await d.learnColor(FULL)).toBe(1) // same colour -> same id
  expect(d.snapshot().learned[1]).toHaveLength(1)
  d.dispose()
})

test('DemoDevice: full table rejects with code full; freed id is reused', async () => {
  const d = new DemoDevice(() => 0)
  const colors = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [0, 255, 255], [255, 0, 255], [255, 128, 0]] as const
  await d.importConfig(
    JSON.stringify({
      version: 1,
      learned: { 1: colors.map((c, i) => ({ id: i + 1, name: '', color: c })), 2: [], 3: [] },
      params: d.snapshot().params,
      system: d.snapshot().system,
    }),
  )
  await d.setParams({ color: { ...d.snapshot().params.color, tolerance: 0 } }) // no existing colour matches exactly
  await expect(d.learnColor(FULL)).rejects.toMatchObject({ code: 'full' })
  await d.forget(1, 3)
  expect(await d.learnColor(FULL)).toBe(3)
  d.dispose()
})

test('DemoDevice defaults to colour function and emits results for it', () => {
  vi.useFakeTimers()
  const d = new DemoDevice(() => 0)
  expect(d.snapshot().activeFunction).toBe(1)
  const batches: { kind: string }[][] = []
  d.onResults((r) => batches.push(r))
  vi.advanceTimersByTime(200)
  expect(batches).toHaveLength(2)
  expect(batches[0].length).toBeGreaterThan(0)
  expect(batches[0].every((r) => r.kind === 'color')).toBe(true)
  d.dispose()
})

test('DemoDevice.learnColor samples the frame last shown, not the current clock', async () => {
  vi.useFakeTimers()
  let clock = 0
  const d = new DemoDevice(() => clock)
  vi.advanceTimersByTime(100) // one tick at clock 0
  const shown = sceneAt(0).find((o) => o.kind === 'blob')!
  clock = 7000 // time moves on, no new frame
  await expect(
    d.learnColor({ x: Math.round(shown.x) - 2, y: Math.round(shown.y) - 2, width: 4, height: 4 }),
  ).resolves.toBe(1)
  d.dispose()
})

test('DemoDevice: re-learning an unchanged colour does not set dirty', async () => {
  const d = new DemoDevice(() => 0)
  await d.learnColor(FULL)
  await d.save()
  await d.learnColor(FULL)
  expect(d.snapshot().dirty).toBe(false)
  d.dispose()
})

test('DemoDevice.setName: no-ops do not dirty; ids and names are validated', async () => {
  const d = new DemoDevice(() => 0)
  await d.setName(1, 3, 'ghost') // colour id that was never learned
  expect(d.snapshot().dirty).toBe(false)
  const id = await d.learnColor(FULL)
  await d.save()
  await d.setName(1, id, '') // unchanged
  expect(d.snapshot().dirty).toBe(false)
  await expect(d.setName(2, 587, 'x')).rejects.toThrow()
  await expect(d.setName(2, -1, 'x')).rejects.toThrow()
  await expect(d.setName(2, 1.5, 'x')).rejects.toThrow()
  await expect(d.setName(2, 5, 'x'.repeat(49))).rejects.toThrow()
  await expect(d.setName(2, 5, '中'.repeat(17))).rejects.toThrow() // 51 UTF-8 bytes
  expect(d.snapshot().learned[2]).toHaveLength(0)
  await d.setName(2, 586, 'edge')
  expect(d.snapshot().learned[2]).toEqual([{ id: 586, name: 'edge' }])
  await d.setName(2, 586, '中'.repeat(16)) // exactly 48 bytes
  await d.setName(2, 586, 'edge')
  await d.save()
  await d.setName(2, 586, 'edge')
  expect(d.snapshot().dirty).toBe(false)
  d.dispose()
})

// ---- M8a: gesture in the demo device ----

test('demo learn on gesture is rejected', async () => {
  const d = new DemoDevice(() => 0)
  await d.setFunction(8)
  await expect(d.learnColor({ x: 0, y: 0, width: 320, height: 240 })).rejects.toBeInstanceOf(LearnError)
  d.dispose()
})

test('demo names gesture ids 1-9 only, and supports function 8', async () => {
  const d = new DemoDevice(() => 0)
  expect(d.snapshot().supported).toEqual([1, 2, 3, 8])
  await d.setName(8, 6, '前進')
  expect(d.snapshot().learned[8]).toEqual([{ id: 6, name: '前進' }])
  await expect(d.setName(8, 0, 'x')).rejects.toThrow()
  await expect(d.setName(8, 10, 'x')).rejects.toThrow()
  d.dispose()
})

test('demo emits gesture results while function 8 is active, and pausing the preview stops frames only', async () => {
  vi.useFakeTimers()
  const d = new DemoDevice(() => 0)
  const frames = vi.fn()
  const results = vi.fn()
  d.onFrame(frames)
  d.onResults(results)
  await d.setFunction(8)
  vi.advanceTimersByTime(500)
  expect(results).toHaveBeenCalled()
  expect(results.mock.calls.every(([r]) => (r as Result[]).every((x) => x.kind === 'gesture'))).toBe(true)
  expect(frames).toHaveBeenCalled()
  await d.setPreviewPaused(true)
  expect(d.snapshot().previewPaused).toBe(true)
  frames.mockClear()
  results.mockClear()
  vi.advanceTimersByTime(500)
  expect(frames).not.toHaveBeenCalled()
  expect(results).toHaveBeenCalled()
  await d.setPreviewPaused(false)
  vi.advanceTimersByTime(300)
  expect(frames).toHaveBeenCalled()
  d.dispose()
})

test('demo gesture params are clamped like the firmware', async () => {
  const d = new DemoDevice(() => 0)
  await d.setParams({ gesture: { handThreshold: 5, gestureThreshold: 150, maxHands: 3 as 1 | 2 } })
  expect(d.snapshot().params.gesture).toEqual({ handThreshold: 10, gestureThreshold: 100, maxHands: 2 })
  d.dispose()
})

test('emptyLearned has function 8 and toFunctionIds keeps 8', () => {
  expect(Object.keys(emptyLearned()).sort()).toEqual(['1', '2', '3', '8'])
  expect(toFunctionIds([1, 4, 8, 9])).toEqual([1, 8])
})
