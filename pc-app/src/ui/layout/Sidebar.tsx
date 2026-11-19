import { Fragment, useId } from 'react'
import { Cpu, Settings } from 'lucide-react'
import { useT, type MessageKey } from '../../i18n'
import CategoryDisc from './CategoryDisc'
import { FN_CATEGORY, FN_ICON } from './categories'
import type { FunctionId, View } from './types'

interface Props {
  view: View
  /** Function IDs the device supports; others are greyed out. */
  supported: FunctionId[]
  /** Real mode without a connection: the hint is "connect first", not "update firmware". */
  needConnect?: boolean
  onSelect(v: View): void
}

const FUNCTIONS: { id: FunctionId; key: MessageKey }[] = [
  { id: 1, key: 'fnColor' },
  { id: 2, key: 'fnTag' },
  { id: 3, key: 'fnFace' },
]

export default function Sidebar({ view, supported, needConnect = false, onSelect }: Props) {
  const { t } = useT()
  const base = useId()
  const hint = t(needConnect ? 'needConnect' : 'needFirmware')
  return (
    <nav className="sidebar" aria-label={t('navFunctions')}>
      {FUNCTIONS.map(({ id, key }) => {
        const ok = !needConnect && supported.includes(id)
        const current = view.kind === 'function' && view.id === id
        return (
          <Fragment key={id}>
            <button
              type="button"
              className="nav-item"
              aria-current={current ? 'page' : undefined}
              aria-disabled={ok ? undefined : true}
              title={ok ? undefined : hint}
              aria-describedby={ok ? undefined : `${base}-${id}`}
              onClick={() => ok && onSelect({ kind: 'function', id })}
            >
              <CategoryDisc cat={FN_CATEGORY[id]} Icon={FN_ICON[id]} />
              <span className="nav-label">{t(key)}</span>
            </button>
            {!ok && (
              <span id={`${base}-${id}`} className="visually-hidden">
                {hint}
              </span>
            )}
          </Fragment>
        )
      })}
      <hr className="nav-rule" />
      <button
        type="button"
        className="nav-item"
        aria-current={view.kind === 'settings' ? 'page' : undefined}
        onClick={() => onSelect({ kind: 'settings' })}
      >
        <CategoryDisc cat="settings" Icon={Settings} />
        <span className="nav-label">{t('navSettings')}</span>
      </button>
      <button
        type="button"
        className="nav-item"
        aria-current={view.kind === 'firmware' ? 'page' : undefined}
        onClick={() => onSelect({ kind: 'firmware' })}
      >
        <CategoryDisc cat="firmware" Icon={Cpu} />
        <span className="nav-label">{t('navFirmware')}</span>
      </button>
    </nav>
  )
}
