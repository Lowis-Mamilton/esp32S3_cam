import { waitFor } from '@testing-library/react'
import { afterEach, expect, test, vi } from 'vitest'
import { Cmd, ErrorCode, parseConfig as parseWireConfig } from '../protocol/messages'
import { FakeFirmware } from '../testing/fakeFirmware'
import { RealDevice } from './RealDevice'
import { LearnError, NotConnectedError, NotSupportedError, defaultParams, type FunctionId, type Result } from './types'

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
  await expect(d.learn(1, { x: 0, y: 0, width: 4, height: 4 })).rejects.toBeInstanceOf(NotConnectedError)
  await expect(d.save()).rejects.toBeInstanceOf(NotConnectedError)
  expect(() => d.exportConfig()).toThrow(NotConnectedError)
})

test('LEARN is answered late (deferred) and still resolves; the region is rounded and clamped', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 1500 // longer than the default 1 s request timeout
  const d = await connected(fw)
  await d.setFunction(1) // LEARN needs the colour function (like the firmware)
  const id = await d.learn(1, { x: 300.4, y: -3, width: 40.6, height: 10.2 })
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
  await d.setFunction(1) // LEARN needs the colour function (like the firmware)
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'full' }))
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toBeInstanceOf(LearnError)
  fw.learnRgb = null
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'no-color' }))
  fw.unknown.add(Cmd.Learn)
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toBeInstanceOf(NotSupportedError)
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
  const changes: (FunctionId | 0)[] = []
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
  // another host switches to function 0 (none): reported as 0 so the UI can say so, not ignored
  fw.externalSetFunction(0)
  fw.pushResults(3, 0, [])
  await waitFor(() => expect(batches).toHaveLength(3))
  expect(changes).toEqual([2, 0])
  expect(d.snapshot().activeFunction).toBe(0)
})

test('no function switch before Studio has sent its own SET_FUNCTION after connecting', async () => {
  const fw = new FakeFirmware()
  fw.active = 2
  const d = await connected(fw, () => 99999)
  const changes: (FunctionId | 0)[] = []
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
  await d.setFunction(1) // LEARN needs the colour function (like the firmware)
  const first = d.learn(1, { x: 0, y: 0, width: 8, height: 8 })
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toEqual(expect.objectContaining({ code: 'busy' }))
  await expect(first).resolves.toBe(1)
})

test('a LEARN cancelled by our own function switch rejects as cancelled, not as "no colour"', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 300
  const d = await connected(fw)
  await d.setFunction(1)
  const learning = d.learn(1, { x: 0, y: 0, width: 8, height: 8 })
  await d.setFunction(2)
  await expect(learning).rejects.toEqual(expect.objectContaining({ code: 'cancelled' }))
})

test('a malformed LEARN reply is an error, not a made-up id', async () => {
  const fw = new FakeFirmware()
  fw.learnDelayMs = 0
  fw.learnReply = new Uint8Array([1, 2])
  const d = await connected(fw)
  await d.setFunction(1) // LEARN needs the colour function (like the firmware)
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toThrow(/length/)
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

// ---- M3: face detection (function 3) ----

test('face: GET_INFO listing 3 makes it supported and selectable; no GET_LEARNED for face', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = await connected(fw)
  expect(d.snapshot().supported).toEqual([1, 2, 3])
  expect(sent(fw, Cmd.GetLearned).map((r) => r.payload[0])).toEqual([1, 2])
  await d.setFunction(3)
  expect(d.snapshot().activeFunction).toBe(3)
  expect(fw.active).toBe(3)
})

test('face: results are kind face with a confidence and id 0', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = await connected(fw)
  await d.setFunction(3)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  fw.pushResults(9, 3, [{ id: 0, x: 100, y: 90, width: 60, height: 70, confidence: 92 }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(got[0]).toMatchObject({ id: 0, kind: 'face', confidence: 92, x: 100, y: 90, width: 60, height: 70 })
})

test('face: threshold and max results go out through SET_CONFIG, and the device value is read back', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = await connected(fw)
  await d.setParams({ face: { threshold: 75, maxResults: 2 } })
  const c = parseWireConfig(sent(fw, Cmd.SetConfig)[0].payload)
  expect([c.faceThreshold, c.faceMaxResults]).toEqual([75, 2])
  expect(d.snapshot().params.face).toEqual({ threshold: 75, maxResults: 2 })
  expect(d.snapshot().dirty).toBe(true)
  // in-range values go through unchanged
  await d.setParams({ face: { threshold: 100, maxResults: 8 } })
  expect(fw.config.faceThreshold).toBe(100)
})

test('face: the fake firmware answers BAD_PARAM to LEARN and SET_NAME for 3 and OK to FORGET', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  fw.learnDelayMs = 0
  const d = await connected(fw)
  await d.setFunction(3)
  await expect(d.learn(1, { x: 0, y: 0, width: 8, height: 8 })).rejects.toBeInstanceOf(LearnError) // LEARN is colour-only
  await expect(d.setName(3, 0, 'x')).rejects.toBeInstanceOf(Error)
  await expect(d.forget(3, 0)).resolves.toBeUndefined()
  expect(fw.config.dirty).toBe(false) // nothing was removed: firmware leaves dirty unchanged
})

// ---- M8a: gesture recognition (function 8) ----

const gestureFw = () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3, 8]
  return fw
}

test('gesture: connect loads gesture func config', async () => {
  const fw = gestureFw()
  fw.gesture = { handThreshold: 30, gestureThreshold: 70, maxHands: 2 }
  const d = await connected(fw)
  expect([...sent(fw, Cmd.GetFuncConfig)[0].payload]).toEqual([8])
  expect(d.snapshot().supported).toEqual([1, 2, 3, 8])
  expect(d.snapshot().params.gesture).toEqual({ handThreshold: 30, gestureThreshold: 70, maxHands: 2 })
  await d.setFunction(8)
  expect(fw.active).toBe(8)
})

test('gesture: setParams sends SET_FUNC_CONFIG and applies the clamped reply', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  await d.setParams({ gesture: { handThreshold: 5, gestureThreshold: 80, maxHands: 2 } })
  expect([...sent(fw, Cmd.SetFuncConfig)[0].payload]).toEqual([8, 1, 10, 80, 2, 0, 0, 0, 0, 0]) // clamped on our side too
  expect(d.snapshot().params.gesture).toEqual({ handThreshold: 10, gestureThreshold: 80, maxHands: 2 })
  expect(d.snapshot().dirty).toBe(true)
  expect(fw.config.dirty).toBe(true)
})

test('gesture: setting the current value sends nothing and stays clean', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  await d.setParams({ gesture: { ...d.snapshot().params.gesture } })
  expect(sent(fw, Cmd.SetFuncConfig)).toHaveLength(0)
  expect(d.snapshot().dirty).toBe(false)
})

test('gesture: results are kind gesture with a confidence', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  await d.setFunction(8)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  fw.pushResults(4, 8, [{ id: 7, x: 120, y: 100, width: 80, height: 90, confidence: 92 }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(got[0]).toMatchObject({ id: 7, kind: 'gesture', confidence: 92, x: 120, y: 100 })
})

test('gesture: names use SET_NAME 8 and GET_LEARNED 8', async () => {
  const fw = gestureFw()
  fw.names.set('8:6', '前進')
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetLearned).map((r) => r.payload[0])).toEqual([1, 2, 8])
  expect(d.snapshot().learned[8]).toEqual([{ id: 6, name: '前進' }])
  await d.setName(8, 9, '後退')
  expect(fw.names.get('8:9')).toBe('後退')
  expect(d.snapshot().learned[8].map((e) => e.id)).toEqual([6, 9])
  await d.forget(8, 6)
  expect(d.snapshot().learned[8].map((e) => e.id)).toEqual([9])
})

test('gesture: old firmware does not get a gesture func config request', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetFuncConfig)).toHaveLength(0)
  expect(d.snapshot().supported).not.toContain(8)
  await expect(d.setFunction(8)).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.setParams({ gesture: { handThreshold: 50, gestureThreshold: 60, maxHands: 1 } })).rejects.toBeInstanceOf(NotSupportedError)
})

test('gesture: func config unsupported drops gesture without failing the connect', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  const fw = gestureFw()
  fw.unknown.add(Cmd.GetFuncConfig)
  const d = await connected(fw)
  expect(d.snapshot().status).toBe('connected')
  expect(d.snapshot().supported).toEqual([1, 2, 3])
  expect(warn).toHaveBeenCalled()
  warn.mockRestore()
})

// ---- pause preview: results keep flowing, FRAME stops ----

test('pause preview: STREAM_START flags 0x02 is sent, results still arrive, no frames; resume sends 0x03', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  const frames: unknown[] = []
  let got: Result[] = []
  d.onFrame((f) => frames.push(f))
  d.onResults((r) => (got = r))
  expect(d.snapshot().previewPaused).toBe(false)
  await d.setPreviewPaused(true)
  const starts = sent(fw, Cmd.StreamStart)
  expect([...starts[starts.length - 1].payload]).toEqual([2, 10, 0])
  expect(d.snapshot().previewPaused).toBe(true)
  fw.pushFrame(1)
  fw.pushResults(1, 8, [{ id: 2, x: 100, y: 100, width: 50, height: 50, confidence: 80 }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(frames).toHaveLength(0)
  await d.setPreviewPaused(false)
  const after = sent(fw, Cmd.StreamStart)
  expect([...after[after.length - 1].payload]).toEqual([3, 10, 0])
  expect(d.snapshot().previewPaused).toBe(false)
  fw.pushFrame(2)
  await waitFor(() => expect(frames).toHaveLength(1))
})

test('pause preview: reconnecting resets it to not paused', async () => {
  let fw = gestureFw()
  const serial = {
    addEventListener: () => {},
    removeEventListener: () => {},
    getPorts: async () => [fw.port],
    requestPort: async () => fw.port,
  } as unknown as Serial
  const d = new RealDevice(serial)
  devices.push(d)
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  await d.setPreviewPaused(true)
  expect(d.snapshot().previewPaused).toBe(true)
  await d.disconnect()
  await waitFor(() => expect(d.snapshot().status).toBe('disconnected'))
  expect(d.snapshot().previewPaused).toBe(false)
  fw = gestureFw() // the board comes back with a fresh stream
  await d.connect()
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  expect(d.snapshot().previewPaused).toBe(false)
  expect([...sent(fw, Cmd.StreamStart)[0].payload]).toEqual([3, 10, 0])
})

test('pause preview: not connected rejects', async () => {
  const d = new RealDevice(undefined)
  devices.push(d)
  await expect(d.setPreviewPaused(true)).rejects.toBeInstanceOf(NotConnectedError)
})

test('gesture: importConfig applies gesture params and names', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  const file = JSON.parse(d.exportConfig())
  file.params.gesture = { handThreshold: 55, gestureThreshold: 75, maxHands: 2 }
  file.learned[8] = [{ id: 7, name: 'OK啦' }]
  await d.importConfig(JSON.stringify(file))
  expect(fw.gesture).toEqual({ handThreshold: 55, gestureThreshold: 75, maxHands: 2 })
  expect(fw.names.get('8:7')).toBe('OK啦')
  expect(d.snapshot().learned[8]).toEqual([{ id: 7, name: 'OK啦' }])
})

test('gesture: importing an old file (no params.gesture) leaves the board gesture params untouched and clean', async () => {
  const fw = gestureFw()
  fw.gesture = { handThreshold: 33, gestureThreshold: 77, maxHands: 2 }
  const d = await connected(fw)
  const file = JSON.parse(d.exportConfig())
  delete file.params.gesture
  delete file.learned[8]
  await d.importConfig(JSON.stringify(file))
  expect(sent(fw, Cmd.SetFuncConfig)).toHaveLength(0)
  expect(fw.gesture).toEqual({ handThreshold: 33, gestureThreshold: 77, maxHands: 2 })
  expect(d.snapshot().params.gesture).toEqual({ handThreshold: 33, gestureThreshold: 77, maxHands: 2 })
})

test('gesture: export omits params.gesture when the board gave no gesture config', async () => {
  const d = await connected() // functions [1, 2]
  expect(JSON.parse(d.exportConfig()).params.gesture).toBeUndefined()
  const d2 = await connected(gestureFw())
  expect(JSON.parse(d2.exportConfig()).params.gesture).toEqual({ handThreshold: 40, gestureThreshold: 60, maxHands: 1 })
})

test('gesture: an unknown func config layout_version drops gesture without failing the connect', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  const fw = gestureFw()
  fw.gestureLayoutVersion = 2
  const d = await connected(fw)
  expect(d.snapshot().status).toBe('connected')
  expect(d.snapshot().supported).toEqual([1, 2, 3])
  warn.mockRestore()
})

test('pause preview: a failed STREAM_START restores the previous paused state', async () => {
  const fw = gestureFw()
  const d = await connected(fw)
  fw.unknown.add(Cmd.StreamStart)
  await expect(d.setPreviewPaused(true)).rejects.toBeDefined()
  expect(d.snapshot().previewPaused).toBe(false)
})

// ---- M6: line tracking (function 4) and object tracking (function 5) ----

const trackFw = () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3, 4, 5]
  return fw
}

test('connect loads func config for 4 and 5', async () => {
  const fw = trackFw()
  fw.line = { mode: 1, threshold: 90, roiPct: 40, bands: 6, minWidth: 3, maxWidth: 60, colorTolerance: 20 }
  fw.track = { threshold: 70, adapt: 5, reacquire: 0, scale: 0 }
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetFuncConfig).map((r) => [...r.payload])).toEqual([[4], [5]])
  expect(d.snapshot().supported).toEqual([1, 2, 3, 4, 5])
  expect(d.snapshot().params.line).toEqual({ mode: 'light', threshold: 90, roiPercent: 40, bands: 6, minWidth: 3, maxWidth: 60, colorTolerance: 20 })
  expect(d.snapshot().params.track).toEqual({ threshold: 70, adapt: 5, reacquire: false, multiScale: false })
})

test('line: setParams sends SET_FUNC_CONFIG and applies the clamped reply', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setParams({ line: { ...d.snapshot().params.line, mode: 'light', roiPercent: 10, bands: 30, minWidth: 20, maxWidth: 5 } })
  expect([...sent(fw, Cmd.SetFuncConfig)[0].payload]).toEqual([4, 1, 1, 0, 20, 12, 20, 20, 30, 0]) // clamped on our side too
  expect(d.snapshot().params.line).toMatchObject({ mode: 'light', roiPercent: 20, bands: 12, minWidth: 20, maxWidth: 20 })
  expect(d.snapshot().dirty).toBe(true)
  expect(fw.config.dirty).toBe(true)
})

test('track: setParams sends SET_FUNC_CONFIG 5; the current value sends nothing', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setParams({ track: { ...d.snapshot().params.track } })
  expect(sent(fw, Cmd.SetFuncConfig)).toHaveLength(0)
  expect(d.snapshot().dirty).toBe(false)
  await d.setParams({ track: { threshold: 99, adapt: 60, reacquire: false, multiScale: true } })
  expect([...sent(fw, Cmd.SetFuncConfig)[0].payload]).toEqual([5, 1, 95, 50, 0, 1, 0, 0, 0, 0])
  expect(d.snapshot().params.track).toEqual({ threshold: 95, adapt: 50, reacquire: false, multiScale: true })
})

test('learn(4) learns the line colour, switches mode to learned and maps BAD_PARAM to no-color', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setFunction(4)
  expect(await d.learn(4, { x: 10, y: 10, width: 40, height: 40 })).toBe(1)
  expect(sent(fw, Cmd.Learn)[0].payload[0]).toBe(4)
  expect(sent(fw, Cmd.Learn)[0].payload).toHaveLength(9) // never the 11-byte form
  expect(d.snapshot().learned[4]).toEqual([{ id: 1, name: '', color: [230, 40, 36] }])
  expect(d.snapshot().params.line.mode).toBe('learned') // re-read after the learn
  fw.learnRgb = null
  await expect(d.learn(4, { x: 10, y: 10, width: 40, height: 40 })).rejects.toMatchObject({ code: 'no-color' })
  await d.forget(4, 1)
  expect(d.snapshot().learned[4]).toEqual([])
  expect(d.snapshot().params.line.mode).toBe('dark')
})

test('forget(4) clears the colour, turns mode learned back to dark but keeps mode light; dirty only on change', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setFunction(4)
  await d.learn(4, { x: 10, y: 10, width: 40, height: 40 })
  fw.line.mode = 1 // user switched to light line while the learned colour is still stored
  fw.config.dirty = false
  await d.forget(4, 0xffff)
  expect(d.snapshot().learned[4]).toEqual([])
  expect(d.snapshot().params.line.mode).toBe('light') // firmware keeps mode 1
  expect(d.snapshot().dirty).toBe(true) // the colour was cleared
  fw.config.dirty = false
  await d.forget(4, 1) // nothing left to forget
  expect(d.snapshot().dirty).toBe(false)
})

test('learn(5) maps BAD_PARAM to no-target and refreshes learned[5]', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setFunction(5)
  fw.learnObjectOk = false
  const err = await d.learn(5, { x: 10, y: 10, width: 8, height: 8 }).catch((e: unknown) => e)
  expect(err).toBeInstanceOf(LearnError)
  expect((err as LearnError).code).toBe('no-target')
  fw.learnObjectOk = true
  expect(await d.learn(5, { x: 10, y: 10, width: 40, height: 40 })).toBe(1)
  expect(d.snapshot().learned[5]).toHaveLength(1)
  expect(d.snapshot().learned[5][0].id).toBe(1)
  await d.setName(5, 1, 'ball')
  expect(d.snapshot().learned[5][0].name).toBe('ball')
  await d.learn(5, { x: 10, y: 10, width: 40, height: 40 }) // learning again replaces the object and its name
  expect(d.snapshot().learned[5][0].name).toBe('')
  await d.forgetAll(5)
  expect(d.snapshot().learned[5]).toEqual([])
})

test('line cannot be named (SET_NAME 4 is rejected)', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await expect(d.setName(4, 1, 'x')).rejects.toThrow()
})

test('line results carry the line extra', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setFunction(4)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  fw.pushResults(9, 4, [
    { id: 1, x: 170, y: 190, width: 21, height: 100, confidence: 100, line: { angleDeg: -12.5, offsetX: 10, start: [170, 240], end: [190, 140], flags: 3 } },
  ])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(got[0].kind).toBe('line')
  expect(got[0].line).toEqual({ angleDeg: -12.5, offsetX: 10, start: [170, 240], end: [190, 140], branch: true, cross: true })
})

test('track results are kind track', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  await d.setFunction(5)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  fw.pushResults(9, 5, [{ id: 1, x: 100, y: 90, width: 40, height: 40, confidence: 88 }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(got[0]).toMatchObject({ id: 1, kind: 'track', confidence: 88 })
})

test('old firmware: no func config requests for 4 and 5', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetFuncConfig)).toHaveLength(0)
  expect(d.snapshot().supported).not.toContain(4)
  expect(d.snapshot().supported).not.toContain(5)
  await expect(d.learn(4, { x: 0, y: 0, width: 10, height: 10 })).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.setParams({ track: { threshold: 50, adapt: 10, reacquire: true, multiScale: true } })).rejects.toBeInstanceOf(NotSupportedError)
})

test('4 and 5 listed but 0x44 unknown: they are dropped without failing the connect', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  const fw = trackFw()
  fw.unknown.add(Cmd.GetFuncConfig)
  const d = await connected(fw)
  expect(d.snapshot().status).toBe('connected')
  expect(d.snapshot().supported).toEqual([1, 2, 3])
  expect(warn).toHaveBeenCalled()
  warn.mockRestore()
})

test('an unknown line layout_version drops only the line', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  const fw = trackFw()
  fw.lineLayoutVersion = 2
  const d = await connected(fw)
  expect(d.snapshot().status).toBe('connected')
  expect(d.snapshot().supported).toEqual([1, 2, 3, 5])
  warn.mockRestore()
})

test('importConfig applies track params and the track name; absent keys leave the board alone', async () => {
  const fw = trackFw()
  fw.line = { ...fw.line, bands: 10 }
  const d = await connected(fw)
  await d.setFunction(5)
  await d.learn(5, { x: 10, y: 10, width: 40, height: 40 })
  const file = JSON.parse(d.exportConfig())
  expect(file.params.line.bands).toBe(10)
  file.params.track = { threshold: 80, adapt: 0, reacquire: false, multiScale: false }
  file.learned[5] = [{ id: 1, name: 'ball' }]
  delete file.params.line
  await d.importConfig(JSON.stringify(file))
  expect(fw.track).toEqual({ threshold: 80, adapt: 0, reacquire: 0, scale: 0 })
  expect(fw.line.bands).toBe(10)
  expect(fw.names.get('5:1')).toBe('ball')
})

// ---- M7: QR code (6), shapes (11), motion (12) ----

const m7Fw = () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3, 4, 5, 6, 8, 11, 12]
  return fw
}
const utf8 = (s: string) => new TextEncoder().encode(s)
const square = (x: number, y: number, s: number): [number, number][] => [
  [x - s / 2, y - s / 2],
  [x + s / 2, y - s / 2],
  [x + s / 2, y + s / 2],
  [x - s / 2, y + s / 2],
]
/** Two codes in front of the camera: 'LEFT' centred at (80, 100), a larger 'MXCam' at (220, 120). */
function twoCodes(fw: FakeFirmware) {
  fw.qrCodes = [
    { record: { id: 0, x: 80, y: 100, width: 41, height: 41, tag: { rotationDeg: 0, corners: square(80, 100, 40) } }, bytes: utf8('LEFT') },
    { record: { id: 0, x: 220, y: 120, width: 51, height: 51, tag: { rotationDeg: 12.5, corners: square(220, 120, 50) } }, bytes: utf8('MXCam') },
  ]
}
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
/** Pushes QR frames until `check` passes on the latest batch (the content arrives a few frames after the box). */
async function pushQrUntil(fw: FakeFirmware, d: RealDevice, firstFrame: number, check: (r: Result[]) => void) {
  let got: Result[] = []
  const off = d.onResults((r) => (got = r))
  let frame = firstFrame
  await waitFor(() => {
    fw.pushQr(frame++)
    check(got)
  })
  off()
  return got
}

test('connect loads func config for 11 and 12', async () => {
  const fw = m7Fw()
  fw.shape = { mode: 1, threshold: 120, minSize: 40, maxResults: 2 }
  fw.motion = { threshold: 50, minSize: 30, maxResults: 3 }
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetFuncConfig).map((r) => r.payload[0])).toEqual([4, 5, 8, 11, 12])
  expect(d.snapshot().supported).toEqual([1, 2, 3, 4, 5, 6, 8, 11, 12])
  expect(d.snapshot().params.shape).toEqual({ mode: 'light', threshold: 120, minSize: 40, maxResults: 2 })
  expect(d.snapshot().params.motion).toEqual({ threshold: 50, minSize: 30, maxResults: 3 })
  // learned lists: QR (6) and shape names (11) are read; motion (12) has none, like face (3)
  expect(sent(fw, Cmd.GetLearned).map((r) => r.payload[0])).toEqual([1, 2, 4, 5, 6, 8, 11])
  const j = JSON.parse(d.exportConfig())
  expect(j.params.shape).toEqual({ mode: 'light', threshold: 120, minSize: 40, maxResults: 2 })
  expect(j.params.motion).toEqual({ threshold: 50, minSize: 30, maxResults: 3 })
})

test('setParams shape and motion send SET_FUNC_CONFIG', async () => {
  const fw = m7Fw()
  const d = await connected(fw)
  await d.setParams({ shape: { ...d.snapshot().params.shape } })
  await d.setParams({ motion: { ...d.snapshot().params.motion } })
  expect(sent(fw, Cmd.SetFuncConfig)).toHaveLength(0) // the current values: nothing sent
  expect(d.snapshot().dirty).toBe(false)
  await d.setParams({ shape: { mode: 'light', threshold: 0, minSize: 5, maxResults: 20 } })
  expect([...sent(fw, Cmd.SetFuncConfig)[0].payload]).toEqual([11, 1, 1, 0, 16, 8, 0, 0, 0, 0]) // clamped on our side too
  expect(d.snapshot().params.shape).toEqual({ mode: 'light', threshold: 0, minSize: 16, maxResults: 8 })
  await d.setParams({ motion: { threshold: 200, minSize: 4, maxResults: 3 } })
  expect([...sent(fw, Cmd.SetFuncConfig)[1].payload]).toEqual([12, 1, 100, 8, 3, 0, 0, 0, 0, 0])
  expect(d.snapshot().params.motion).toEqual({ threshold: 100, minSize: 8, maxResults: 3 })
  expect(fw.motion).toEqual({ threshold: 100, minSize: 8, maxResults: 3 })
  expect(d.snapshot().dirty).toBe(true)
})

test('qr results get text through GET_RESULT_DATA', async () => {
  const fw = m7Fw()
  twoCodes(fw)
  let clock = 0
  const d = await connected(fw, () => clock)
  await d.setFunction(6)
  const got = await pushQrUntil(fw, d, 10, (r) => expect(r.map((x) => x.text)).toEqual(['LEFT', 'MXCam']))
  expect(got[1]).toMatchObject({ id: 0, kind: 'qr', x: 220, y: 120, width: 51, height: 51, rotationDeg: 12.5, text: 'MXCam', truncated: false })
  expect(got[1].corners).toEqual(square(220, 120, 50))
  expect(got[1].confidence).toBeUndefined()
  expect(sent(fw, Cmd.GetResultData).map((r) => r.payload[0])).toEqual([0, 1]) // one read of the batch
  // all codes matched: no more reads within a second, however many frames come
  for (let f = 50; f < 60; f++) fw.pushQr(f)
  await sleep(30)
  expect(sent(fw, Cmd.GetResultData)).toHaveLength(2)
  // a second later the contents are read once more (a card swapped in place)
  clock = 1500
  fw.pushQr(60)
  await waitFor(() => expect(sent(fw, Cmd.GetResultData)).toHaveLength(4))
})

test('qr text: long and invalid UTF-8 content is shown truncated with replacement characters', async () => {
  const fw = m7Fw()
  const long = new Uint8Array(200).fill(0x41)
  long[1] = 0xff
  fw.qrCodes = [{ record: { id: 0, x: 100, y: 100, width: 60, height: 60, tag: { rotationDeg: 0, corners: square(100, 100, 60) } }, bytes: long }]
  const d = await connected(fw)
  await d.setFunction(6)
  const [r] = await pushQrUntil(fw, d, 1, (b) => expect(b[0]?.text).toBeDefined())
  expect(r.truncated).toBe(true)
  expect(r.text).toBe(`A�${'A'.repeat(126)}`)
})

test('stale frame id discards text', async () => {
  const fw = m7Fw()
  twoCodes(fw)
  const d = await connected(fw, () => 0)
  await d.setFunction(6)
  const batches: { frameId: number; texts: (string | undefined)[] }[] = []
  d.onResults((r, m) => batches.push({ frameId: m!.frameId, texts: r.map((x) => x.text) }))
  fw.pushQr(10)
  fw.nextFrame(11) // the board finishes frame 11 before Studio asks about frame 10
  await waitFor(() => expect(sent(fw, Cmd.GetResultData)).toHaveLength(1))
  await sleep(30)
  expect(sent(fw, Cmd.GetResultData)).toHaveLength(1) // dropped at the first mismatch: index 1 is not asked
  fw.pushQr(12)
  await waitFor(() => expect(batches).toHaveLength(2))
  expect(batches[1].texts).toEqual([undefined, undefined]) // frame 11's contents were never attached
  await pushQrUntil(fw, d, 13, (r) => expect(r.map((x) => x.text)).toEqual(['LEFT', 'MXCam']))
})

test('QR listed but GET_RESULT_DATA unknown: boxes without text, asked only once', async () => {
  const fw = m7Fw()
  twoCodes(fw)
  fw.unknown.add(Cmd.GetResultData)
  const d = await connected(fw)
  await d.setFunction(6)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  for (let f = 1; f <= 5; f++) {
    fw.pushQr(f)
    await sleep(10)
  }
  expect(got.map((r) => r.kind)).toEqual(['qr', 'qr'])
  expect(got.every((r) => r.text === undefined)).toBe(true)
  expect(sent(fw, Cmd.GetResultData)).toHaveLength(1)
})

test('learn(6) maps BAD_PARAM to no-code and FULL to full', async () => {
  const fw = m7Fw()
  twoCodes(fw)
  fw.learnDelayMs = 0
  const d = await connected(fw)
  await d.setFunction(6)
  const err = await d.learn(6, { x: 0, y: 0, width: 20, height: 20 }).catch((e: unknown) => e)
  expect(err).toBeInstanceOf(LearnError)
  expect((err as LearnError).code).toBe('no-code')
  fw.qrLearned = [1, 2, 3, 4, 5, 6, 7, 8].map((id) => ({ id, bytes: utf8(`other${id}`) }))
  await expect(d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).rejects.toMatchObject({ code: 'full' })
  fw.unknown.add(Cmd.Learn)
  await expect(d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).rejects.toBeInstanceOf(NotSupportedError)
})

test('learn(6) refreshes learned[6] with text', async () => {
  const fw = m7Fw()
  twoCodes(fw)
  fw.learnDelayMs = 0
  const d = await connected(fw)
  await d.setFunction(6)
  expect(await d.learn(6, { x: 60, y: 80, width: 40, height: 40 })).toBe(1) // the region holds the centre of 'LEFT'
  expect(sent(fw, Cmd.Learn)[0].payload).toHaveLength(9) // never the 11-byte form
  expect(d.snapshot().learned[6]).toEqual([{ id: 1, name: '', text: 'LEFT' }])
  expect(await d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).toBe(2) // the whole frame: the larger code
  expect(await d.learn(6, { x: 0, y: 0, width: 320, height: 240 })).toBe(2) // same content: same ID
  await d.setName(6, 2, '起點')
  expect(d.snapshot().learned[6]).toEqual([
    { id: 1, name: '', text: 'LEFT' },
    { id: 2, name: '起點', text: 'MXCam' },
  ])
  expect(sent(fw, Cmd.GetLearnedData).at(-1)!.payload[0]).toBe(6)
  await d.forget(6, 1)
  expect(d.snapshot().learned[6].map((l) => l.id)).toEqual([2])
  // results carry the learned ID from the board
  fw.qrCodes[1].record.id = 2
  await pushQrUntil(fw, d, 1, (r) => expect(r.map((x) => [x.id, x.text])).toEqual([[0, 'LEFT'], [2, 'MXCam']]))
})

test('connect reads the QR contents of every learned ID; a firmware without 0x35 still connects', async () => {
  const fw = m7Fw()
  fw.qrLearned = [
    { id: 3, bytes: utf8('右轉') },
    { id: 1, bytes: utf8('LEFT') },
  ]
  fw.names.set('6:3', 'right')
  const d = await connected(fw)
  expect(sent(fw, Cmd.GetLearnedData).map((r) => [...r.payload])).toEqual([
    [6, 1, 0],
    [6, 3, 0],
  ])
  expect(d.snapshot().learned[6]).toEqual([
    { id: 1, name: '', text: 'LEFT' },
    { id: 3, name: 'right', text: '右轉' },
  ])
  const fw2 = m7Fw()
  fw2.qrLearned = [{ id: 1, bytes: utf8('LEFT') }]
  fw2.unknown.add(Cmd.GetLearnedData)
  const d2 = await connected(fw2)
  expect(d2.snapshot().learned[6]).toEqual([{ id: 1, name: '' }])
})

test('shape and motion results kinds', async () => {
  const fw = m7Fw()
  const d = await connected(fw)
  let got: Result[] = []
  d.onResults((r) => (got = r))
  await d.setFunction(11)
  fw.pushResults(5, 11, [{ id: 3, x: 100, y: 90, width: 50, height: 50, confidence: 91 }])
  await waitFor(() => expect(got).toHaveLength(1))
  expect(got[0]).toEqual({ id: 3, kind: 'shape', x: 100, y: 90, width: 50, height: 50, confidence: 91 })
  await d.setFunction(12)
  fw.pushResults(6, 12, [{ id: 0, x: 200, y: 120, width: 60, height: 40, confidence: 63 }])
  await waitFor(() => expect(got[0]?.kind).toBe('motion'))
  expect(got[0]).toEqual({ id: 0, kind: 'motion', x: 200, y: 120, width: 60, height: 40, confidence: 63 })
  expect(sent(fw, Cmd.GetResultData)).toHaveLength(0) // only QR results have data
  // shape names 1-4; motion cannot be named
  await d.setName(11, 4, '長方形')
  expect(d.snapshot().learned[11]).toEqual([{ id: 4, name: '長方形' }])
  await expect(d.setName(11, 5, 'x')).rejects.toThrow()
  await expect(d.setName(12, 1, 'x')).rejects.toThrow()
  await d.forgetAll(12) // nothing to forget: OK like face
})

test('old firmware: no m7 requests', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3, 4, 5, 8]
  const d = await connected(fw)
  expect(d.snapshot().supported).toEqual([1, 2, 3, 4, 5, 8])
  await expect(d.setFunction(6)).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.learn(6, { x: 0, y: 0, width: 10, height: 10 })).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.setParams({ shape: defaultParams().shape })).rejects.toBeInstanceOf(NotSupportedError)
  await expect(d.setParams({ motion: defaultParams().motion })).rejects.toBeInstanceOf(NotSupportedError)
  const file = JSON.parse(d.exportConfig())
  expect(file.params.shape).toBeUndefined()
  expect(file.params.motion).toBeUndefined()
  file.params.shape = { mode: 'light', threshold: 0, minSize: 32, maxResults: 4 }
  file.learned[6] = [{ id: 1, name: 'x', text: 'LEFT' }]
  file.learned[11] = [{ id: 1, name: '圓' }]
  expect((await d.importConfig(JSON.stringify(file))).colorsNeedRelearn).toBe(false)
  expect(sent(fw, Cmd.GetResultData)).toHaveLength(0)
  expect(sent(fw, Cmd.GetLearnedData)).toHaveLength(0)
  expect(sent(fw, Cmd.GetFuncConfig).map((r) => r.payload[0])).not.toContain(11)
  expect(sent(fw, Cmd.GetFuncConfig).map((r) => r.payload[0])).not.toContain(12)
  expect(sent(fw, Cmd.SetFuncConfig).map((r) => r.payload[0])).not.toContain(11)
  expect(sent(fw, Cmd.SetName).map((r) => r.payload[0])).not.toContain(11)
  expect(sent(fw, Cmd.GetLearned).map((r) => r.payload[0])).not.toContain(6)
})

test('func config unsupported drops shape', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3, 11, 12]
  fw.unknown.add(Cmd.GetFuncConfig)
  const d = await connected(fw)
  expect(d.snapshot().status).toBe('connected')
  expect(d.snapshot().supported).toEqual([1, 2, 3])
  // an unknown layout_version drops only that function
  const fw2 = m7Fw()
  fw2.shapeLayoutVersion = 2
  const d2 = await connected(fw2)
  expect(d2.snapshot().supported).toEqual([1, 2, 3, 4, 5, 6, 8, 12])
  warn.mockRestore()
})

test('importConfig applies shape and motion params and shape names; absent keys leave the board alone', async () => {
  const fw = m7Fw()
  fw.motion = { threshold: 40, minSize: 24, maxResults: 4 }
  fw.names.set('11:2', 'old')
  const d = await connected(fw)
  const file = JSON.parse(d.exportConfig())
  file.params.shape = { mode: 'light', threshold: 90, minSize: 48, maxResults: 2 }
  delete file.params.motion
  file.learned[11] = [{ id: 1, name: '圓' }]
  await d.importConfig(JSON.stringify(file))
  expect(fw.shape).toEqual({ mode: 1, threshold: 90, minSize: 48, maxResults: 2 })
  expect(fw.motion.threshold).toBe(40)
  expect(fw.names.get('11:1')).toBe('圓')
  expect(fw.names.has('11:2')).toBe(false) // the file's shape names replace the board's
  expect(d.snapshot().learned[11]).toEqual([{ id: 1, name: '圓' }])
})

test('importConfig reports a learned line colour that cannot be restored', async () => {
  const fw = trackFw()
  const d = await connected(fw)
  const file = JSON.parse(d.exportConfig())
  file.learned[4] = [{ id: 1, name: '', color: [1, 2, 3] }]
  expect((await d.importConfig(JSON.stringify(file))).colorsNeedRelearn).toBe(true)
})
