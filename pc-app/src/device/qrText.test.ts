import { expect, test, vi } from 'vitest'
import { QrTextCache, decodeQrText, type QrRead } from './qrText'
import type { Result } from './types'

const qr = (x: number, y: number): Result => ({ id: 0, kind: 'qr', x, y, width: 40, height: 40 })
const flush = () => new Promise((r) => setTimeout(r, 0))

/** A fetch answering from a table of contents per index, for frame `frame.id`. */
function fakeFetch(texts: string[], frame = { id: 1 }) {
  return vi.fn(async (i: number): Promise<QrRead> => ({ frameId: frame.id, text: texts[i], truncated: false }))
}

test('attaches text by nearest centre', async () => {
  const fetch = fakeFetch(['LEFT', 'MXCam'])
  const c = new QrTextCache(fetch)
  expect(c.annotate([qr(60, 60), qr(200, 120)], 1, 0).map((r) => r.text)).toEqual([undefined, undefined])
  await flush()
  expect(fetch.mock.calls.map((a) => a[0])).toEqual([0, 1])
  // next batch: the order changed and the codes moved a little; each record gets the content of the nearest known code
  const out = c.annotate([qr(205, 118), qr(64, 57)], 2, 100)
  expect(out.map((r) => r.text)).toEqual(['MXCam', 'LEFT'])
  expect(out.map((r) => r.truncated)).toEqual([false, false])
  // a record farther than 32 px from every known code gets nothing
  expect(c.annotate([qr(64, 57), qr(300, 200)], 3, 200)[1].text).toBeUndefined()
})

test('the known position follows the record, so a slowly moving code keeps its text', async () => {
  const fetch = fakeFetch(['A'])
  const c = new QrTextCache(fetch)
  c.annotate([qr(100, 100)], 1, 0)
  await flush()
  let x = 100
  for (let t = 100; t < 900; t += 100) {
    x += 20 // 160 px in total, 20 px per batch
    expect(c.annotate([qr(x, 100)], 2, t)[0].text).toBe('A')
  }
  expect(fetch).toHaveBeenCalledTimes(1)
})

test('unmatched record triggers a fetch', async () => {
  const fetch = fakeFetch(['A', 'B'])
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60)], 1, 0)
  await flush()
  expect(fetch).toHaveBeenCalledTimes(1)
  c.annotate([qr(60, 60), qr(200, 120)], 1, 50) // a second code appears: read right away, not after a second
  await flush()
  expect(fetch).toHaveBeenCalledTimes(3)
  expect(c.annotate([qr(60, 60), qr(200, 120)], 1, 100).map((r) => r.text)).toEqual(['A', 'B'])
})

test('fetch is throttled to once per second when all match', async () => {
  const fetch = fakeFetch(['A'])
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60)], 1, 0)
  await flush()
  for (let t = 100; t < 1000; t += 100) {
    c.annotate([qr(60, 60)], 1, t)
    await flush()
  }
  expect(fetch).toHaveBeenCalledTimes(1)
  c.annotate([qr(60, 60)], 1, 1000) // a card swapped in place shows its new content within a second
  await flush()
  expect(fetch).toHaveBeenCalledTimes(2)
  // no records: nothing to read
  c.annotate([], 1, 5000)
  await flush()
  expect(fetch).toHaveBeenCalledTimes(2)
})

test('stale frame id discards text', async () => {
  const frame = { id: 8 } // the device finished frame 8 before the request came: it answers about another batch
  const fetch = fakeFetch(['WRONG', 'WRONG'], frame)
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60), qr(200, 120)], 7, 0)
  await flush()
  expect(fetch).toHaveBeenCalledTimes(1) // dropped at the first mismatch
  expect(c.annotate([qr(60, 60), qr(200, 120)], 8, 100).map((r) => r.text)).toEqual([undefined, undefined])
  await flush() // still unmatched: this batch is read (and its frame id matches)
  expect(c.annotate([qr(60, 60), qr(200, 120)], 9, 200).map((r) => r.text)).toEqual(['WRONG', 'WRONG'])
})

test('a failed read (lost reply, BAD_PARAM) is retried with a later batch', async () => {
  const fetch = vi.fn<(i: number) => Promise<QrRead>>().mockRejectedValueOnce(new Error('timeout'))
  fetch.mockResolvedValue({ frameId: 2, text: 'OK', truncated: true })
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60)], 1, 0)
  await flush()
  c.annotate([qr(60, 60)], 2, 100)
  await flush()
  expect(c.annotate([qr(60, 60)], 3, 200)[0]).toMatchObject({ text: 'OK', truncated: true })
})

test('qr text with invalid utf-8', () => {
  expect(decodeQrText(new Uint8Array([0x41, 0xff, 0x42]))).toBe('A�B')
  expect(decodeQrText(new TextEncoder().encode('左轉'))).toBe('左轉')
  expect(decodeQrText(new Uint8Array([0xe5, 0xb7]))).toBe('�') // cut in the middle of a character (truncated data)
  expect(decodeQrText(new Uint8Array(0))).toBe('')
})

test('only one fetch in flight', async () => {
  let release!: (r: QrRead) => void
  const fetch = vi.fn(() => new Promise<QrRead>((res) => (release = res)))
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60)], 1, 0)
  c.annotate([qr(60, 60)], 2, 100)
  c.annotate([qr(60, 60)], 3, 2000)
  expect(fetch).toHaveBeenCalledTimes(1)
  release({ frameId: 1, text: 'A', truncated: false })
  await flush()
  expect(c.annotate([qr(60, 60)], 4, 2100)[0].text).toBe('A')
})

test('reset forgets the contents and drops a read in flight', async () => {
  let release!: (r: QrRead) => void
  const fetch = vi.fn(() => new Promise<QrRead>((res) => (release = res)))
  const c = new QrTextCache(fetch)
  c.annotate([qr(60, 60)], 1, 0)
  c.reset()
  release({ frameId: 1, text: 'OLD', truncated: false })
  await flush()
  expect(c.annotate([qr(60, 60)], 1, 10)[0].text).toBeUndefined()
  expect(fetch).toHaveBeenCalledTimes(2) // reset also allows a new read at once
})
