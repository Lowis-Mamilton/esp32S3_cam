import { waitFor } from '@testing-library/react'
import { afterEach, expect, test, vi } from 'vitest'
import { Cmd, ErrorCode, parseConfig as parseWireConfig } from '../protocol/messages'
import { FakeFirmware } from '../testing/fakeFirmware'
import { RealDevice } from './RealDevice'
import { LearnError, NotConnectedError, NotSupportedError, type FunctionId, type Result } from './types'

let devices: RealDevice[] = []
afterEach(() => {
  devices.forEach((d) => d.dispose())
  devices = []
  vi.useRealTimers()
})

async function connected(fw = new FakeFirmware(), now?: () => number) {
  const d = new RealDevice(fw.serial(), now)
  devices.push(d)
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  return d
}
const sent = (fw: FakeFirmware, cmd: number) => fw.requests.filter((r) => r.cmd === cmd)

test('connect reads info, config and learned data before streaming frames and results', async () => {
  const fw = new FakeFirmware()
  fw.config = { ...fw.config, tolerance: 45, minArea: 250, colorShowNames: false, i2cAddress: 0x30, ledBrightness: 20, dirty: true }
  fw.colors = [{ id: 2, rgb: [10, 200, 30] }]
  fw.names.set('1:2', '綠色積木')
  fw.names.set('2:7', '起點')
  const d = await connected(fw)
  expect(fw.cmds().slice(0, 5)).toEqual([Cmd.GetInfo, Cmd.GetConfig, Cmd.GetLearned, Cmd.GetLearned, Cmd.StreamStart])
  expect(sent(fw, Cmd.GetLearned).map((r) => r.payload[0])).toEqual([1, 2])
  expect([...sent(fw, Cmd.StreamStart)[0].payload]).toEqual([3, 10, 0])
  const s = d.snapshot()
  expect(s.supported).toEqual([1, 2])
  expect(s.configKnown).toBe(true)
  expect(s.dirty).toBe(true)
  expect(s.params.color).toEqual({ tolerance: 45, minArea: 250, maxResults: 4, showNames: false })
  expect(s.system).toEqual({ uartBaud: 115200, i2cAddress: 0x30, ledBrightness: 20 })
  expect(s.learned[1]).toEqual([{ id: 2, name: '綠色積木', color: [10, 200, 30] }])
  expect(s.learned[2]).toEqual([{ id: 7, name: '起點' }])
})

test('face stays unsupported on firmware that lists [1, 2]', async () => {
  const d = await connected()
  expect(d.snapshot().supported).not.toContain(3)
  await expect(d.setFunction(3)).rejects.toBeInstanceOf(NotSupportedError)
})

test('operations before connecting reject with NotConnectedError', async () => {
  const d = new RealDevice(undefined)
  devices.push(d)
  await expect(d.setFunction(1)).rejects.toBeInstanceOf(NotConnectedError)
  await expect(d.learnColor({ x: 0, y: 0, width: 4, height: 4 })).rejects.toBeInstanceOf(NotConnectedError)
  await expect(d.save()).rejects.toBeInstanceOf(NotConnectedError)
  expect(() => d.exportConfig()).toThrow(NotConnectedError)
})

test('LEARN is answered late (deferred) and still resolves; the region is rounded and clamped', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 1500 // longer than the default 1 s request timeout
  const d = await connected(fw)
  const id = await d.learnColor({ x: 300.4, y: -3, width: 40.6, height: 10.2 })
  expect(id).toBe(1)
  const p = sent(fw, Cmd.Learn)[0].payload
  const v = new DataView(p.buffer, p.byteOffset)
  expect([p[0], v.getUint16(1, true), v.getUint16(3, true), v.getUint16(5, true), v.getUint16(7, true)]).toEqual([1, 300, 0, 20, 10])
  expect(d.snapshot().learned[1]).toEqual([{ id: 1, name: '', color: [230, 40, 36] }])
  expect(d.snapshot().dirty).toBe(true)
})

test('LEARN errors: FULL -> LearnError full, BAD_PARAM -> LearnError no-color, UNKNOWN_CMD -> NotSupportedError', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 0
  fw.colors = [1, 2, 3, 4, 5, 6, 7].map((id) => ({ id, rgb: [id, 0, 0] as [number, number, number] }))
  const d = await connected(fw)
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'full' }))
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toBeInstanceOf(LearnError)
  fw.learnRgb = null
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'no-color' }))
  fw.unknown.add(Cmd.Learn)
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toBeInstanceOf(NotSupportedError)
})

test('forget, forgetAll and setName go over the protocol and refresh the learned list', async () => {
  const fw = new FakeFirmware()
  fw.colors = [
    { id: 1, rgb: [255, 0, 0] },
    { id: 2, rgb: [0, 0, 255] },
  ]
  const d = await connected(fw)
  await d.setName(1, 2, '藍色')
  expect(d.snapshot().learned[1].find((l) => l.id === 2)?.name).toBe('藍色')
  await d.setName(2, 586, 'edge')
  expect(d.snapshot().learned[2]).toEqual([{ id: 586, name: 'edge' }])
  await d.forget(1, 1)
  expect(d.snapshot().learned[1].map((l) => l.id)).toEqual([2])
  const f = sent(fw, Cmd.Forget)[0].payload
  expect([...f]).toEqual([1, 1, 0])
  await d.forgetAll(2)
  expect([...sent(fw, Cmd.Forget)[1].payload]).toEqual([2, 0xff, 0xff])
  expect(d.snapshot().learned[2]).toEqual([])
  expect(d.snapshot().dirty).toBe(true)
})

test('setName checks the 48-byte UTF-8 limit before sending; FULL name table is a typed error', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  await expect(d.setName(2, 1, '中'.repeat(17))).rejects.toThrow(/48/)
  expect(sent(fw, Cmd.SetName)).toHaveLength(0)
  await d.setName(2, 1, '中'.repeat(16))
  expect(d.snapshot().learned[2][0].name).toBe('中'.repeat(16))
  fw.failWith.set(Cmd.SetName, ErrorCode.Full)
  await expect(d.setName(2, 2, 'x')).rejects.toEqual(expect.objectContaining({ code: 'names-full' }))
})

test('setParams / setSystem send the whole config and apply the clamped config the device returns', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  await d.setParams({ color: { ...d.snapshot().params.color, tolerance: 70 } })
  const c = parseWireConfig(sent(fw, Cmd.SetConfig)[0].payload)
  expect(c.tolerance).toBe(70)
  expect(c.minArea).toBe(100)
  expect(c.i2cAddress).toBe(0x5a)
  expect(d.snapshot().params.color.tolerance).toBe(70)
  expect(d.snapshot().dirty).toBe(true)
  await d.setSystem({ ledBrightness: 150 }) // device clamps to 100
  expect(d.snapshot().system.ledBrightness).toBe(100)
  const before = sent(fw, Cmd.SetConfig).length
  await d.setSystem({ ledBrightness: 100 }) // no change: nothing sent
  expect(sent(fw, Cmd.SetConfig)).toHaveLength(before)
})

test('older firmware without config commands: connects, config unknown, config edits are NotSupported', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = []
  for (const c of [Cmd.GetConfig, Cmd.SetConfig, Cmd.GetLearned, Cmd.Save, Cmd.Learn]) fw.unknown.add(c)
  const d = await connected(fw)
  expect(d.snapshot().configKnown).toBe(false)
  await expect(d.setParams({})).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.save()).rejects.toBeInstanceOf(NotSupportedError)
  expect(() => d.exportConfig()).toThrow(NotSupportedError)
})

test('save waits for the flash write and refreshes dirty; factoryReset reloads everything', async () => {
  const fw = new FakeFirmware()
  fw.saveDelayMs = 1500 // longer than the default 1 s timeout
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  fw.config.dirty = true
  const d = await connected(fw)
  await d.save()
  expect(d.snapshot().dirty).toBe(false)
  await d.setSystem({ ledBrightness: 5 })
  await d.factoryReset()
  const s = d.snapshot()
  expect(s.learned[1]).toEqual([])
  expect(s.system.ledBrightness).toBe(50)
  expect(s.dirty).toBe(false)
}, 10000)

test('BUSY and other device errors become a readable error, not NotSupported', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  fw.failWith.set(Cmd.Save, ErrorCode.Busy)
  const e = await d.save().then(
    () => null,
    (x: unknown) => x,
  )
  expect(e).toBeInstanceOf(Error)
  expect(e).not.toBeInstanceOf(NotSupportedError)
  expect((e as Error).message).toMatch(/BUSY/)
})

test('setFunction sends SET_FUNCTION and records the active function', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  await d.setFunction(2)
  expect(sent(fw, Cmd.SetFunction).map((r) => r.payload[0])).toEqual([2])
  expect(fw.active).toBe(2)
  expect(d.snapshot().activeFunction).toBe(2)
})

test('RESULTS pushes become Result[] with frameId, functionId and AprilTag extras; JPEG frames carry frameId', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  await d.setFunction(2)
  const got: { results: Result[]; meta?: { frameId: number; functionId: number } }[] = []
  const frames: (number | undefined)[] = []
  d.onResults((results, meta) => got.push({ results, meta }))
  d.onFrame((f) => frames.push(f.kind === 'jpeg' ? f.frameId : undefined))
  const corners: [number, number][] = [
    [10, 10],
    [50, 10],
    [50, 50],
    [10, 50],
  ]
  fw.pushFrame(41)
  fw.pushResults(41, 2, [{ id: 3, x: 30, y: 30, width: 40, height: 40, tag: { rotationDeg: -12.5, corners } }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(frames).toEqual([41])
  expect(got[0].meta).toEqual({ frameId: 41, functionId: 2 })
  expect(got[0].results).toEqual([
    { id: 3, kind: 'tag', x: 30, y: 30, width: 40, height: 40, rotationDeg: -12.5, corners },
  ])
})

test('function ownership: a RESULTS push for another function switches, except within 1 s of our own SET_FUNCTION', async () => {
  let clock = 0
  const fw = new FakeFirmware()
  const d = await connected(fw, () => clock)
  const changes: FunctionId[] = []
  d.onFunctionChange((fn) => changes.push(fn))
  const batches: number[] = []
  d.onResults((_, m) => batches.push(m?.frameId ?? -1))
  await d.setFunction(1)
  // stale in-flight frames of the previous function, within 1 s: ignored
  clock = 500
  fw.pushResults(1, 2, [])
  await waitFor(() => expect(batches).toEqual([1]))
  expect(changes).toEqual([])
  expect(d.snapshot().activeFunction).toBe(1)
  // the R4 changes the function later
  clock = 1600
  fw.externalSetFunction(2)
  fw.pushResults(2, 2, [])
  await waitFor(() => expect(changes).toEqual([2]))
  expect(d.snapshot().activeFunction).toBe(2)
  // function 0 (none) never switches the view
  fw.pushResults(3, 0, [])
  await waitFor(() => expect(batches).toHaveLength(3))
  expect(changes).toEqual([2])
})

test('no function switch before Studio has sent its own SET_FUNCTION after connecting', async () => {
  const fw = new FakeFirmware()
  fw.active = 2
  const d = await connected(fw, () => 99999)
  const changes: FunctionId[] = []
  d.onFunctionChange((fn) => changes.push(fn))
  const batches: number[] = []
  d.onResults((_, m) => batches.push(m?.frameId ?? -1))
  fw.pushResults(5, 2, [])
  await waitFor(() => expect(batches).toEqual([5]))
  expect(changes).toEqual([])
})

test('exportConfig reads the snapshot in the demo file format', async () => {
  const fw = new FakeFirmware()
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  fw.names.set('1:1', 'red')
  const d = await connected(fw)
  const j = JSON.parse(d.exportConfig())
  expect(j.version).toBe(1)
  expect(j.learned[1]).toEqual([{ id: 1, name: 'red', color: [255, 0, 0] }])
  expect(j.params.color.tolerance).toBe(30)
  expect(j.system.i2cAddress).toBe(0x5a)
})

test('importConfig applies config and names only, and reports colours that must be re-learned', async () => {
  const fw = new FakeFirmware()
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  fw.names.set('2:9', 'old')
  const d = await connected(fw)
  const file = {
    version: 1,
    learned: {
      1: [
        { id: 1, name: '紅', color: [255, 0, 0] },
        { id: 2, name: '藍', color: [0, 0, 255] },
      ],
      2: [{ id: 3, name: '起點' }],
      3: [],
    },
    params: { ...d.snapshot().params, color: { ...d.snapshot().params.color, tolerance: 55 } },
    system: { ...d.snapshot().system, ledBrightness: 80 },
  }
  const out = await d.importConfig(JSON.stringify(file))
  expect(out).toEqual({ colorsNeedRelearn: true })
  expect(sent(fw, Cmd.Learn)).toHaveLength(0)
  const s = d.snapshot()
  expect(s.params.color.tolerance).toBe(55)
  expect(s.system.ledBrightness).toBe(80)
  expect(s.learned[1]).toEqual([{ id: 1, name: '紅', color: [255, 0, 0] }])
  expect(s.learned[2]).toEqual([{ id: 3, name: '起點' }])
  // same colours already on the board: nothing to re-learn
  const again = await d.importConfig(JSON.stringify({ ...file, learned: { ...file.learned, 1: [file.learned[1][0]] } }))
  expect(again).toEqual({ colorsNeedRelearn: false })
})

test('importConfig validates before touching the device', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  const n = fw.requests.length
  await expect(d.importConfig('{"version":1}')).rejects.toThrow(/invalid config/)
  expect(fw.requests.slice(n).filter((r) => r.cmd !== Cmd.Ping)).toHaveLength(0)
})

test('disconnect clears the device state', async () => {
  const fw = new FakeFirmware()
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  const d = await connected(fw)
  expect(d.snapshot().learned[1]).toHaveLength(1)
  await d.disconnect()
  const s = d.snapshot()
  expect(s.status).toBe('disconnected')
  expect(s.learned[1]).toEqual([])
  expect(s.configKnown).toBe(false)
  expect(s.activeFunction).toBe(0)
})

test('concurrent config edits do not undo each other (each is merged into the config the previous one left)', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  await Promise.all([d.setSystem({ ledBrightness: 10 }), d.setParams({ color: { ...d.snapshot().params.color, tolerance: 80 } })])
  expect(fw.config.ledBrightness).toBe(10)
  expect(fw.config.tolerance).toBe(80)
  expect(d.snapshot().system.ledBrightness).toBe(10)
  expect(d.snapshot().params.color.tolerance).toBe(80)
})

// ---- review fix round ----
test('connect retries GET_CONFIG and GET_LEARNED once when a reply is lost', async () => {
  const fw = new FakeFirmware()
  fw.drop.set(Cmd.GetConfig, 1)
  fw.drop.set(Cmd.GetLearned, 1)
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  const d = new RealDevice(fw.serial())
  devices.push(d)
  await waitFor(() => expect(d.snapshot().status).toBe('connected'), { timeout: 4000 })
  expect(d.snapshot().configKnown).toBe(true)
  expect(d.snapshot().learned[1]).toHaveLength(1)
  expect(sent(fw, Cmd.GetConfig)).toHaveLength(2)
}, 10000)

test('a second LEARN while one is pending is BUSY -> LearnError busy; the first still succeeds', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 300
  const d = await connected(fw)
  const first = d.learnColor({ x: 0, y: 0, width: 8, height: 8 })
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'busy' }))
  await expect(first).resolves.toBe(1)
})

test('a LEARN cancelled by our own function switch rejects as cancelled, not as "no colour"', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 300
  const d = await connected(fw)
  await d.setFunction(1)
  const learning = d.learnColor({ x: 0, y: 0, width: 8, height: 8 })
  await d.setFunction(2)
  await expect(learning).rejects.toEqual(expect.objectContaining({ code: 'cancelled' }))
})

test('a malformed LEARN reply is an error, not a made-up id', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 0
  fw.learnReply = new Uint8Array([1, 2])
  const d = await connected(fw)
  await expect(d.learnColor({ x: 0, y: 0, width: 8, height: 8 })).rejects.toThrow(/length/)
})

test('out-of-order replies settle the right operations', async () => {
  const fw = new FakeFirmware()
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  const d = await connected(fw)
  fw.replyDelay.set(Cmd.SetConfig, 200)
  await Promise.all([d.setSystem({ ledBrightness: 10 }), d.setName(1, 1, 'red')])
  expect(d.snapshot().system.ledBrightness).toBe(10)
  expect(d.snapshot().learned[1][0].name).toBe('red')
})

test('a failed refresh after a successful operation still resolves and is retried in the background', async () => {
  const fw = new FakeFirmware()
  const d = await connected(fw)
  fw.failWith.set(Cmd.GetLearned, ErrorCode.Busy)
  await expect(d.setName(2, 4, 'four')).resolves.toBeUndefined()
  fw.failWith.delete(Cmd.GetLearned)
  await waitFor(() => expect(d.snapshot().learned[2]).toEqual([{ id: 4, name: 'four' }]), { timeout: 3000 })
})

test('save and factoryReset wait for queued config edits and later edits wait for them', async () => {
  const fw = new FakeFirmware()
  fw.saveDelayMs = 300
  const d = await connected(fw)
  const reset = d.factoryReset()
  const edit = d.setSystem({ ledBrightness: 10 })
  await Promise.all([reset, edit])
  expect(fw.config.ledBrightness).toBe(10)
  expect(d.snapshot().system.ledBrightness).toBe(10)
})

test('import checks the 32-name table before wiping tag names', async () => {
  const fw = new FakeFirmware()
  fw.names.set('2:9', 'keep')
  const d = await connected(fw)
  const file = JSON.parse(d.exportConfig())
  file.learned[2] = Array.from({ length: 33 }, (_, i) => ({ id: i, name: `t${i}` }))
  await expect(d.importConfig(JSON.stringify(file))).rejects.toEqual(expect.objectContaining({ code: 'names-full' }))
  expect(sent(fw, Cmd.Forget)).toHaveLength(0)
  expect(fw.names.get('2:9')).toBe('keep')
})
