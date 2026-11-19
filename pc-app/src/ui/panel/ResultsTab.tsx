import { useSyncExternalStore } from 'react'
import type { ResultsStore } from '../../device/resultsStore'
import type { LearnedId, Result } from '../../device/types'
import { useT } from '../../i18n'
import type { FunctionId } from '../layout/types'
import { idColorVar } from '../live/labels'

const KIND: Record<FunctionId, Result['kind']> = { 1: 'color', 2: 'tag', 3: 'face' }

interface Props {
  fn: FunctionId
  /** The newest results: what the R4 would read. */
  results: ResultsStore
  learned: LearnedId[]
}

export default function ResultsTab({ fn, results: store, learned }: Props) {
  const { t } = useT()
  const results = useSyncExternalStore(store.subscribe, store.latest)
  const rows = results.filter((r) => r.kind === KIND[fn])
  if (rows.length === 0) return <p className="muted">{t('resultsEmpty')}</p>
  const face = fn === 3
  const idText = (r: Result) => (r.kind === 'face' ? t('catFace') : r.kind === 'tag' ? `Tag ${r.id}` : r.id ? `ID${r.id}` : '—')
  return (
    <table className="rp-table">
      <thead>
        <tr>
          <th scope="col">{t('colId')}</th>
          <th scope="col">{t('colName')}</th>
          <th scope="col">x</th>
          <th scope="col">y</th>
          <th scope="col">{t('colWidth')}</th>
          <th scope="col">{t('colHeight')}</th>
          {face && <th scope="col">{t('colConfidence')}</th>}
        </tr>
      </thead>
      <tbody>
        {rows.map((r, i) => (
          <tr key={i}>
            <td>
              <span className="rp-dot" style={{ background: idColorVar(r) }} aria-hidden="true" />
              {idText(r)}
            </td>
            <td className="rp-name">{learned.find((l) => l.id === r.id)?.name ?? ''}</td>
            <td>{Math.round(r.x)}</td>
            <td>{Math.round(r.y)}</td>
            <td>{Math.round(r.width)}</td>
            <td>{Math.round(r.height)}</td>
            {face && <td>{Math.round(r.confidence ?? 0)}%</td>}
          </tr>
        ))}
      </tbody>
    </table>
  )
}
