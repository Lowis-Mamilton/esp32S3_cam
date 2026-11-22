import { Fragment, useEffect, useId, useRef } from 'react'
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
  /** A greyed-out function was clicked or tapped: the hint a mouse gets by hovering (touch has no hover). */
  onUnavailable?(hint: MessageKey): void
}

const FUNCTIONS: { id: FunctionId; key: MessageKey }[] = [
  { id: 1, key: 'fnColor' },
  { id: 2, key: 'fnTag' },
  { id: 3, key: 'fnFace' },
  { id: 4, key: 'fnLine' },
  { id: 5, key: 'fnTrack' },
  { id: 8, key: 'fnGesture' },
]

/** Scrolls only the list itself (a rail when wide, a sideways strip when narrow), never the page. */
function keepVisible(box: HTMLElement, item: HTMLElement) {
  const b = box.getBoundingClientRect()
  const r = item.getBoundingClientRect()
  const pad = 8 // a little of the neighbour stays in sight, so it is clear the list goes on
  if (r.left < b.left + pad) box.scrollLeft -= b.left + pad - r.left
  else if (r.right > b.right - pad) box.scrollLeft += r.right - (b.right - pad)
  if (r.top < b.top + pad) box.scrollTop -= b.top + pad - r.top
  else if (r.bottom > b.bottom - pad) box.scrollTop += r.bottom - (b.bottom - pad)
}

export default function Sidebar({ view, supported, needConnect = false, onSelect, onUnavailable }: Props) {
  const { t } = useT()
  const base = useId()
  const hintKey: MessageKey = needConnect ? 'needConnect' : 'needFirmware'
  const hint = t(hintKey)
  const navRef = useRef<HTMLElement>(null)
  // The current item stays in sight, also when another device (the R4) switched the function.
  const currentKey = view.kind === 'function' ? view.id : view.kind
  useEffect(() => {
    const nav = navRef.current
    const item = nav?.querySelector<HTMLElement>('[aria-current="page"]')
    if (nav && item) keepVisible(nav, item)
  }, [currentKey])
  return (
    <nav ref={navRef} className="sidebar" aria-label={t('navFunctions')}>
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
              onClick={() => (ok ? onSelect({ kind: 'function', id }) : onUnavailable?.(hintKey))}
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
