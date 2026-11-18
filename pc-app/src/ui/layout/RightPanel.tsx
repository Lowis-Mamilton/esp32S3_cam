import { useId, useRef, useState, type KeyboardEvent } from 'react'
import { useT, type MessageKey } from '../../i18n'
import type { PanelTab } from './types'

const TABS: { id: PanelTab; key: MessageKey }[] = [
  { id: 'results', key: 'tabResults' },
  { id: 'learn', key: 'tabLearn' },
  { id: 'params', key: 'tabParams' },
  { id: 'code', key: 'tabCode' },
]

export default function RightPanel() {
  const { t } = useT()
  const [tab, setTab] = useState<PanelTab>('results')
  const base = useId()
  const refs = useRef<Record<PanelTab, HTMLButtonElement | null>>({ results: null, learn: null, params: null, code: null })

  const onKeyDown = (e: KeyboardEvent, index: number) => {
    let next: number
    if (e.key === 'ArrowRight') next = (index + 1) % TABS.length
    else if (e.key === 'ArrowLeft') next = (index - 1 + TABS.length) % TABS.length
    else if (e.key === 'Home') next = 0
    else if (e.key === 'End') next = TABS.length - 1
    else return
    e.preventDefault()
    const id = TABS[next].id
    setTab(id)
    refs.current[id]?.focus()
  }
  return (
    <aside className="rightpanel">
      <div role="tablist" className="tabs">
        {TABS.map(({ id, key }, i) => (
          <button
            key={id}
            ref={(el) => {
              refs.current[id] = el
            }}
            type="button"
            role="tab"
            id={`${base}-${id}`}
            aria-selected={tab === id}
            tabIndex={tab === id ? 0 : -1}
            onKeyDown={(e) => onKeyDown(e, i)}
            aria-controls={`${base}-panel`}
            onClick={() => setTab(id)}
          >
            {t(key)}
          </button>
        ))}
      </div>
      {/* Task 5 fills the tab contents. */}
      <div role="tabpanel" id={`${base}-panel`} aria-labelledby={`${base}-${tab}`} className="tabpanel">
        <p className="muted">{t('panelEmpty')}</p>
      </div>
    </aside>
  )
}
