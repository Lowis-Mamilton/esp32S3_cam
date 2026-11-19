import { Fragment, useId } from 'react'
import { Cpu, Palette, QrCode, ScanFace, Settings, type LucideIcon } from 'lucide-react'
import { useT, type MessageKey } from '../../i18n'
import type { FunctionId, View } from './types'

interface Props {
  view: View
  /** Function IDs the device supports; others are greyed out. */
  supported: FunctionId[]
  /** Real mode without a connection: the hint is "connect first", not "update firmware". */
  needConnect?: boolean
  onSelect(v: View): void
}

const FUNCTIONS: { id: FunctionId; key: MessageKey; Icon: LucideIcon }[] = [
  { id: 1, key: 'fnColor', Icon: Palette },
  { id: 2, key: 'fnTag', Icon: QrCode },
  { id: 3, key: 'fnFace', Icon: ScanFace },
]

export default function Sidebar({ view, supported, needConnect = false, onSelect }: Props) {
  const { t } = useT()
  const base = useId()
  const hint = t(needConnect ? 'needConnect' : 'needFirmware')
  return (
    <nav className="sidebar" aria-label={t('navFunctions')}>
      {FUNCTIONS.map(({ id, key, Icon }) => {
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
              <Icon size={18} aria-hidden="true" />
              {t(key)}
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
        <Settings size={18} aria-hidden="true" />
        {t('navSettings')}
      </button>
      <button
        type="button"
        className="nav-item"
        aria-current={view.kind === 'firmware' ? 'page' : undefined}
        onClick={() => onSelect({ kind: 'firmware' })}
      >
        <Cpu size={18} aria-hidden="true" />
        {t('navFirmware')}
      </button>
    </nav>
  )
}
