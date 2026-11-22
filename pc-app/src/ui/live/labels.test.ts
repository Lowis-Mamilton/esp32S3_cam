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

test('gesture: language name or user name, plus confidence', () => {
  const g = (id: number, confidence: number): Result => ({ id, kind: 'gesture', confidence, ...box })
  expect(labelFor(g(6, 92), [], true, tz)).toBe('讚 92%')
  expect(labelFor(g(6, 92), [{ id: 6, name: '前進' }], true, tz)).toBe('前進 92%')
  expect(labelFor(g(0, 81), [], true, tz)).toBe('手 81%')
  expect(labelFor(g(7, 70), [], true, te)).toBe('OK 70%')
})

test('gesture colours: id 0 unlearned, ids cycle 8 colours', () => {
  expect(idColorVar({ id: 0, kind: 'gesture', ...box })).toBe('var(--unlearned)')
  expect(idColorVar({ id: 9, kind: 'gesture', ...box })).toBe('var(--id-1)')
  expect(idColorVar({ id: 3, kind: 'gesture', ...box })).toBe('var(--id-3)')
})

test('line label: signed angle and offset with a real minus sign, localised', () => {
  const r: Result = { id: 1, kind: 'line', ...box, line: { angleDeg: -8, offsetX: 20, start: [0, 0], end: [0, 0], branch: false, cross: false } }
  expect(labelFor(r, [], false, tz)).toBe('角度 −8°・偏移 +20')
  expect(labelFor(r, [], false, te)).toBe('Angle −8°・Offset +20')
  expect(labelFor({ ...r, line: { ...r.line!, angleDeg: 0, offsetX: 0 } }, [], false, tz)).toBe('角度 0°・偏移 0')
})

test('track label: learned name, or 物體, plus confidence', () => {
  const r: Result = { id: 1, kind: 'track', ...box, confidence: 92.4 }
  expect(labelFor(r, [], true, tz)).toBe('物體 92%')
  expect(labelFor(r, [{ id: 1, name: '小車' }], true, tz)).toBe('小車 92%')
  expect(labelFor(r, [], true, te)).toBe('Object 92%')
})

test('line and track have their own colours', () => {
  expect(idColorVar({ id: 1, kind: 'line', ...box })).toBe('var(--cat-line)')
  expect(idColorVar({ id: 1, kind: 'track', ...box })).toBe('var(--cat-track)')
})

// ---- M7: QR code (6), shapes (11), motion (12) ----
test('QR label: QR when not learned, ID n when learned, the name when it has one', () => {
  const q = (id: number): Result => ({ id, kind: 'qr', ...box, text: 'LEFT' })
  expect(labelFor(q(0), [], true, tz)).toBe('QR')
  expect(labelFor(q(2), [{ id: 2, name: '', text: 'LEFT' }], true, tz)).toBe('ID 2')
  expect(labelFor(q(2), [{ id: 2, name: '左轉', text: 'LEFT' }], true, tz)).toBe('左轉')
  expect(labelFor(q(2), [], true, te)).toBe('ID 2')
})

test('shape label: shape name or the given name, plus confidence', () => {
  const s = (id: number, confidence: number): Result => ({ id, kind: 'shape', confidence, ...box })
  expect(labelFor(s(2, 88), [], true, tz)).toBe('三角形 88%')
  expect(labelFor(s(1, 92.4), [], true, te)).toBe('Circle 92%')
  expect(labelFor(s(4, 85), [], true, tz)).toBe('長方形 85%')
  expect(labelFor(s(2, 88), [{ id: 2, name: '屋頂' }], true, tz)).toBe('屋頂 88%')
})

test('motion label: 動態 plus confidence', () => {
  const r: Result = { id: 0, kind: 'motion', confidence: 63, ...box }
  expect(labelFor(r, [], true, tz)).toBe('動態 63%')
  expect(labelFor(r, [], true, te)).toBe('Motion 63%')
})

test('QR, shape and motion colours', () => {
  expect(idColorVar({ id: 0, kind: 'qr', ...box })).toBe('var(--unlearned)')
  expect(idColorVar({ id: 3, kind: 'qr', ...box })).toBe('var(--id-3)')
  expect(idColorVar({ id: 1, kind: 'shape', ...box })).toBe('var(--id-1)')
  expect(idColorVar({ id: 4, kind: 'shape', ...box })).toBe('var(--id-4)')
  expect(idColorVar({ id: 0, kind: 'motion', ...box })).toBe('var(--cat-motion)')
})
