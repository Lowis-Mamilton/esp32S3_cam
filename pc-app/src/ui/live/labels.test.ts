import { expect, test } from 'vitest'
import { translate } from '../../i18n/core'
import type { LearnedId, Result } from '../../device/types'
import { idColorVar, labelFor } from './labels'

const tz = (k: Parameters<typeof translate>[1], v?: Record<string, string | number>) => translate('zh-TW', k, v)
const te = (k: Parameters<typeof translate>[1], v?: Record<string, string | number>) => translate('en', k, v)
const box = { x: 0, y: 0, width: 10, height: 10 }
const learned: LearnedId[] = [
  { id: 1, name: '紅色積木' },
  { id: 3, name: '起點' },
  { id: 4, name: '' },
]

test('learned colour: ID plus name, or ID alone', () => {
  expect(labelFor({ id: 1, kind: 'color', ...box }, learned, true, tz)).toBe('ID1 紅色積木')
  expect(labelFor({ id: 4, kind: 'color', ...box }, learned, true, tz)).toBe('ID4')
  expect(labelFor({ id: 1, kind: 'color', ...box }, learned, false, tz)).toBe('ID1')
})

test('tag', () => {
  expect(labelFor({ id: 3, kind: 'tag', ...box }, learned, true, tz)).toBe('Tag 3 起點')
  expect(labelFor({ id: 3, kind: 'tag', ...box }, learned, false, tz)).toBe('Tag 3')
  expect(labelFor({ id: 5, kind: 'tag', ...box }, learned, true, tz)).toBe('Tag 5')
})

test('face shows percentage, localised', () => {
  const r: Result = { id: 0, kind: 'face', ...box, confidence: 92 }
  expect(labelFor(r, [], true, tz)).toBe('人臉 92%')
  expect(labelFor(r, [], true, te)).toBe('Face 92%')
})

test('unlearned shows the category name', () => {
  const r: Result = { id: 0, kind: 'color', ...box }
  expect(labelFor(r, learned, true, tz)).toBe('顏色')
  expect(labelFor(r, learned, true, te)).toBe('Color')
})

test('idColorVar is stable per id and unlearned is distinct', () => {
  expect(idColorVar({ id: 1, kind: 'color', ...box })).toBe('var(--id-1)')
  expect(idColorVar({ id: 8, kind: 'color', ...box })).toBe('var(--id-8)')
  expect(idColorVar({ id: 3, kind: 'tag', ...box })).toBe('var(--id-4)')
  expect(idColorVar({ id: 10, kind: 'tag', ...box })).toBe('var(--id-4)')
  for (let i = 0; i < 50; i++) expect(idColorVar({ id: i, kind: 'tag', ...box })).not.toBe('var(--id-8)')
  expect(idColorVar({ id: 0, kind: 'color', ...box })).toBe('var(--unlearned)')
  expect(idColorVar({ id: 0, kind: 'face', ...box })).toBe('var(--id-8)')
})
