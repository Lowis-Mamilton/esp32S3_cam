import { useId, useState } from 'react'
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
  return (
    <aside className="rightpanel">
      <div role="tablist" className="tabs">
        {TABS.map(({ id, key }) => (
          <button
            key={id}
            type="button"
            role="tab"
            id={`${base}-${id}`}
            aria-selected={tab === id}
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
