import { expect, test } from 'vitest'
import { en } from './en'
import { describeError, translate } from './core'
import { zhTW } from './zh-TW'

test('both dictionaries have exactly the same keys', () => {
  expect(Object.keys(en).sort()).toEqual(Object.keys(zhTW).sort())
})

test('no dictionary value is empty', () => {
  for (const d of [en, zhTW]) for (const v of Object.values(d)) expect(v.trim()).not.toBe('')
})

test('translate substitutes variables', () => {
  expect(translate('en', 'items', { n: 3 })).toBe('3 items')
  expect(translate('zh-TW', 'items', { n: 3 })).toBe('共 3 項')
})

test('describeError maps known failures and keeps detail for unknown ones', () => {
  expect(describeError(Object.assign(new Error('x'), { name: 'NetworkError' })).key).toBe('errPortBusy')
  expect(describeError(Object.assign(new Error('x'), { name: 'InvalidStateError' })).key).toBe('errAlreadyOpen')
  expect(describeError(new Error('request timeout')).key).toBe('errNoResponse')
  expect(describeError(new Error('boom'))).toEqual({ key: 'errGeneric', detail: 'boom' })
})
