import type { LearnErrorCode, LearnedId, Result } from '../../device/types'
import type { MessageKey, TFn } from '../../i18n'

/** Message for a LearnError, shared by the live view and the panels. */
export const LEARN_ERROR_KEY: Record<LearnErrorCode, MessageKey> = {
  'no-color': 'learnNoColor',
  full: 'learnFull',
  busy: 'learnBusy',
  cancelled: 'learnCancelled',
}

/** Label text per docs/studio-ui.md section 3. `learned` is the list of the result's own function. */
export function labelFor(r: Result, learned: LearnedId[], showNames: boolean, t: TFn): string {
  const name = showNames ? (learned.find((l) => l.id === r.id)?.name ?? '') : ''
  const join = (head: string) => (name ? `${head} ${name}` : head)
  if (r.kind === 'face') return `${t('catFace')} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'gesture') return `${name || t(`gesture${r.id}` as MessageKey)} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'tag') return join(`Tag ${r.id}`)
  if (r.id === 0) return t('catColor')
  return join(`ID${r.id}`)
}

export function idColorVar(r: Result): string {
  // Tags cycle ids 1-7; --id-8 is reserved for faces.
  if (r.kind === 'face') return 'var(--id-8)'
  if (r.kind === 'gesture') return r.id === 0 ? 'var(--unlearned)' : `var(--id-${((r.id - 1) % 8) + 1})`
  if (r.kind === 'tag') return `var(--id-${(r.id % 7) + 1})`
  if (r.id === 0) return 'var(--unlearned)'
  return `var(--id-${((r.id - 1) % 8) + 1})`
}
