import type { LearnedId, Result } from '../../device/types'
import type { TFn } from '../../i18n'

/** Label text per docs/studio-ui.md section 3. `learned` is the list of the result's own function. */
export function labelFor(r: Result, learned: LearnedId[], showNames: boolean, t: TFn): string {
  const name = showNames ? (learned.find((l) => l.id === r.id)?.name ?? '') : ''
  const join = (head: string) => (name ? `${head} ${name}` : head)
  if (r.kind === 'face') return `${t('catFace')} ${Math.round(r.confidence ?? 0)}%`
  if (r.kind === 'tag') return join(`Tag ${r.id}`)
  if (r.id === 0) return t('catColor')
  return join(`ID${r.id}`)
}

export function idColorVar(r: Result): string {
  if (r.kind === 'face') return 'var(--id-4)'
  if (r.kind === 'tag') return `var(--id-${(r.id % 8) + 1})`
  if (r.id === 0) return 'var(--unlearned)'
  return `var(--id-${((r.id - 1) % 8) + 1})`
}
