import { expect, test } from 'vitest'
import { ConfigFileError, parseConfig } from './config'
import { defaultParams, defaultSystem, emptyLearned } from './types'

const base = () => ({ version: 1, learned: emptyLearned(), params: defaultParams(), system: defaultSystem() })
type Cfg = ReturnType<typeof base>
const make = (mut: (c: Cfg) => void) => {
  const c = base()
  mut(c)
  return JSON.stringify(c)
}

test('accepts the defaults and boundary values', () => {
  const c = parseConfig(JSON.stringify(base()))
  expect(c.params).toEqual(defaultParams())
  expect(c.system).toEqual(defaultSystem())
  const full = make((c) => {
    c.learned[1] = [{ id: 1, name: 'red', color: [255, 0, 0] }]
    c.learned[2] = [
      { id: 0, name: 't0' },
      { id: 586, name: 't586' },
    ]
    c.learned[3] = [{ id: 1, name: 'a'.repeat(48) }]
    c.learned[2].push({ id: 5, name: '中'.repeat(16) }) // 48 bytes
    c.params.color.tolerance = 0
    c.params.face.threshold = 100
    c.system.i2cAddress = 0x77
    c.system.uartBaud = 9600
  })
  expect(() => parseConfig(full)).not.toThrow()
})

const bad: [string, (c: Cfg) => void][] = [
  ['tolerance > 100', (c) => (c.params.color.tolerance = 101)],
  ['tolerance < 0', (c) => (c.params.color.tolerance = -1)],
  ['minArea negative', (c) => (c.params.color.minArea = -1)],
  ['minArea fractional', (c) => (c.params.color.minArea = 1.5)],
  ['color maxResults 0', (c) => (c.params.color.maxResults = 0)],
  ['color maxResults 9', (c) => (c.params.color.maxResults = 9)],
  ['color maxResults fractional', (c) => (c.params.color.maxResults = 2.5)],
  ['face threshold 101', (c) => (c.params.face.threshold = 101)],
  ['face maxResults 0', (c) => (c.params.face.maxResults = 0)],
  ['face maxResults 9', (c) => (c.params.face.maxResults = 9)],
  ['uartBaud 57600', (c) => ((c.system as { uartBaud: number }).uartBaud = 57600)],
  ['i2c 0x07', (c) => (c.system.i2cAddress = 0x07)],
  ['i2c 0x78', (c) => (c.system.i2cAddress = 0x78)],
  ['i2c fractional', (c) => (c.system.i2cAddress = 20.5)],
  ['led 101', (c) => (c.system.ledBrightness = 101)],
  ['led -1', (c) => (c.system.ledBrightness = -1)],
  ['color id 0', (c) => (c.learned[1] = [{ id: 0, name: 'x' }])],
  ['color id 8', (c) => (c.learned[1] = [{ id: 8, name: 'x' }])],
  [
    'color id duplicate',
    (c) =>
      (c.learned[1] = [
        { id: 1, name: 'a' },
        { id: 1, name: 'b' },
      ]),
  ],
  ['color id fractional', (c) => (c.learned[1] = [{ id: 1.5, name: 'a' }])],
  ['tag id 587', (c) => (c.learned[2] = [{ id: 587, name: 'a' }])],
  ['tag id -1', (c) => (c.learned[2] = [{ id: -1, name: 'a' }])],
  [
    'tag id duplicate',
    (c) =>
      (c.learned[2] = [
        { id: 5, name: 'a' },
        { id: 5, name: 'b' },
      ]),
  ],
  ['name too long', (c) => (c.learned[1] = [{ id: 1, name: 'a'.repeat(49) }])],
  ['name over 48 UTF-8 bytes (17 CJK chars)', (c) => (c.learned[2] = [{ id: 1, name: '中'.repeat(17) }])],
  ['rgb 256', (c) => (c.learned[1] = [{ id: 1, name: 'a', color: [256, 0, 0] }])],
  ['rgb negative', (c) => (c.learned[1] = [{ id: 1, name: 'a', color: [0, -1, 0] }])],
  ['rgb fractional', (c) => (c.learned[1] = [{ id: 1, name: 'a', color: [0, 0.5, 0] }])],
]
test.each(bad)('rejects out-of-range config: %s', (_name, mut) => {
  expect(() => parseConfig(make(mut))).toThrow(Error)
})

test('rejects non-JSON, wrong version and wrong shapes', () => {
  for (const s of ['nope', '{}', '{"version":2}', 'null', '[]']) expect(() => parseConfig(s)).toThrow(Error)
})

// ---- M8a: gesture in the config file ----

test('an old file without params.gesture or learned[8] imports with no gesture params', () => {
  const raw = JSON.parse(JSON.stringify(base()))
  delete raw.params.gesture
  delete raw.learned[8]
  const c = parseConfig(JSON.stringify(raw))
  expect(c.params.gesture).toBeUndefined()
  expect(c.learned[8]).toEqual([])
})

test('gesture params and names round-trip; limits are enforced', () => {
  const c = parseConfig(
    make((c) => {
      c.params.gesture = { handThreshold: 95, gestureThreshold: 0, maxHands: 2 }
      c.learned[8] = [
        { id: 1, name: 'one' },
        { id: 9, name: '倒讚' },
      ]
    }),
  )
  expect(c.params.gesture).toEqual({ handThreshold: 95, gestureThreshold: 0, maxHands: 2 })
  expect(c.learned[8].map((e) => e.id)).toEqual([1, 9])
  expect(() => parseConfig(make((c) => void (c.params.gesture.maxHands = 3 as 1 | 2)))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.params.gesture.maxHands = 0 as 1 | 2)))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.params.gesture.handThreshold = 9)))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.params.gesture.gestureThreshold = 101)))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.learned[8] = [{ id: 0, name: 'x' }])))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.learned[8] = [{ id: 10, name: 'x' }])))).toThrow(ConfigFileError)
  expect(() => parseConfig(make((c) => void (c.learned[8] = [{ id: 1, name: 'a' }, { id: 1, name: 'b' }])))).toThrow(ConfigFileError)
})
