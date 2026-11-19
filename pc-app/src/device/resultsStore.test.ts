import { expect, test, vi } from 'vitest'
import { ResultsStore } from './resultsStore'
import type { Result } from './types'

const r = (id: number): Result[] => [{ id, kind: 'color', x: 1, y: 1, width: 2, height: 2 }]

test('without frame ids (demo) both views show the latest batch', () => {
  const s = new ResultsStore()
  expect(s.latest()).toEqual([])
  s.push(r(1))
  s.push(r(2))
  expect(s.latest()).toEqual(r(2))
  expect(s.paired()).toEqual(r(2))
})

test('review point 4: paired() is the newest batch not later than the displayed frame', () => {
  const s = new ResultsStore()
  s.push(r(10), 10)
  s.push(r(11), 11)
  s.push(r(12), 12)
  expect(s.paired()).toEqual(r(12)) // nothing displayed yet: latest
  s.setDisplayed(11)
  expect(s.paired()).toEqual(r(11))
  expect(s.latest()).toEqual(r(12))
  s.setDisplayed(13) // results for 13 not here yet: keep 12
  expect(s.paired()).toEqual(r(12))
  s.push(r(13), 13)
  expect(s.paired()).toEqual(r(13))
})

test('a displayed frame older than every kept batch shows nothing rather than boxes from another frame', () => {
  const s = new ResultsStore()
  for (let i = 100; i < 140; i++) s.push(r(i), i)
  s.setDisplayed(50)
  expect(s.paired()).toEqual([])
})

test('notifies only when a view changes; snapshots are stable between changes', () => {
  const s = new ResultsStore()
  const cb = vi.fn()
  s.subscribe(cb)
  s.push(r(1), 1)
  expect(cb).toHaveBeenCalledTimes(1)
  const a = s.paired()
  expect(s.paired()).toBe(a)
  s.setDisplayed(1) // same batch is still selected
  expect(cb).toHaveBeenCalledTimes(1)
  s.clear()
  expect(cb).toHaveBeenCalledTimes(2)
  expect(s.latest()).toEqual([])
  expect(s.paired()).toEqual([])
})
