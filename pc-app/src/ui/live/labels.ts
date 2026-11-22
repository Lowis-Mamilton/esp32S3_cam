import type { LearnErrorCode, LearnedId, Result } from '../../device/types'
import type { MessageKey, TFn } from '../../i18n'
import type { FunctionId } from '../layout/types'

/** Result kind each function produces (a Record: a new function must be handled, never fall through). */
export const FN_KIND: Record<FunctionId, Result['kind']> = {
  1: 'color', 2: 'tag', 3: 'face', 4: 'line', 5: 'track', 6: 'qr', 8: 'gesture', 11: 'shape', 12: 'motion',
}
/** Hat-block text above the live picture. */
export const STAGE_KEY: Record<FunctionId, MessageKey> = {
  1: 'stageColor', 2: 'stageTag', 3: 'stageFace', 4: 'stageLine', 5: 'stageTrack', 6: 'stageQr', 8: 'stageGesture', 11: 'stageShape', 12: 'stageMotion',
}
/** Drag hint under the picture, for the functions that learn. */
export const DRAG_HINT_KEY: Partial<Record<FunctionId, MessageKey>> = { 1: 'dragHint', 4: 'dragHintLine', 5: 'dragHintTrack', 6: 'dragHintQr' }
export const LEARNED_KEY: Partial<Record<FunctionId, MessageKey>> = { 1: 'learned', 4: 'learnedLine', 5: 'learnedTrack', 6: 'learnedQr' }

const MINUS = '−'
/** Signed integer with a real minus sign: -8 -> "−8", 20 -> "+20", 0 -> "0". */
export const signed = (n: number) => {
  const v = Math.round(n)
  return v < 0 ? `${MINUS}${-v}` : v > 0 ? `+${v}` : '0'
}

/** Message for a LearnError, shared by the live view and the panels (a full QR table has its own: learnErrorKey). */
export const LEARN_ERROR_KEY: Record<LearnErrorCode, MessageKey> = {
  'no-color': 'learnNoColor',
  'no-target': 'learnNoTarget',
  'no-code': 'learnErrorNoCode',
  full: 'learnFull',
  busy: 'learnBusy',
  cancelled: 'learnCancelled',
}

/** Shape ids 1-4 (circle, triangle, square, rectangle); a newer firmware's id 5+ is just "shape". */
export const SHAPE_IDS = [1, 2, 3, 4] as const
export const shapeName = (id: number, t: TFn): string => ((SHAPE_IDS as readonly number[]).includes(id) ? t(`shape${id}` as MessageKey) : t('catShape'))

/** Label text per docs/studio-ui.md section 3. `learned` is the list of the result's own function. */
export function labelFor(r: Result, learned: LearnedId[], showNames: boolean, t: TFn): string {
  const name = showNames ? (learned.find((l) => l.id === r.id)?.name ?? '') : ''
  const join = (head: string) => (name ? `${head} ${name}` : head)
  if (r.kind === 'line') return `${t('lineAngle')} ${signed(r.line?.angleDeg ?? 0)}°・${t('lineOffset')} ${signed(r.line?.offsetX ?? 0)}`
  if (r.kind === 'track') return `${name || t('catObject')} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'face') return `${t('catFace')} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'gesture') return `${name || t(`gesture${r.id}` as MessageKey)} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'tag') return join(`Tag ${r.id}`)
  if (r.kind === 'qr') return name || (r.id ? `ID ${r.id}` : t('qrUnlearned'))
  if (r.kind === 'shape') return `${name || shapeName(r.id, t)} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'motion') return `${t('motionLabel')} ${Math.round(r.confidence ?? 0)}%`
  if (r.id === 0) return t('catColor')
  return join(`ID${r.id}`)
}

export function idColorVar(r: Result): string {
  // Tags cycle ids 1-7; --id-8 is reserved for faces.
  if (r.kind === 'face') return 'var(--id-8)'
  if (r.kind === 'gesture') return r.id === 0 ? 'var(--unlearned)' : `var(--id-${((r.id - 1) % 8) + 1})`
  if (r.kind === 'line') return 'var(--cat-line)'
  if (r.kind === 'track') return 'var(--cat-track)'
  if (r.kind === 'motion') return 'var(--cat-motion)'
  if (r.kind === 'tag') return `var(--id-${(r.id % 7) + 1})`
  if (r.id === 0) return 'var(--unlearned)'
  return `var(--id-${((r.id - 1) % 8) + 1})`
}

/** The message for a learn rejection of function `fn`: 'full' means 8 QR codes for function 6, 7 colours otherwise. */
export const learnErrorKey = (fn: FunctionId, code: LearnErrorCode): MessageKey => (fn === 6 && code === 'full' ? 'learnFullQr' : LEARN_ERROR_KEY[code])

/** At most this many characters of a QR code's content go on the picture; the results tab shows all of it. */
const QR_TEXT_ON_STAGE = 16
/** The QR content shown beside the label: the first 16 characters, with "…" when there is more (or the code was cut). */
export function qrStageText(r: Result): string | null {
  if (!r.text) return null
  const chars = [...r.text]
  const cut = chars.length > QR_TEXT_ON_STAGE
  return chars.slice(0, QR_TEXT_ON_STAGE).join('') + (cut || r.truncated ? '…' : '')
}
