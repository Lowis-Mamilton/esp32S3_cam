import { useId, useRef, useState, type KeyboardEvent } from 'react'
import type { ResultsStore } from '../../device/resultsStore'
import { lacksConfig, needsConnection, type Device, type DeviceSnapshot } from '../../device/types'
import { useT, type MessageKey } from '../../i18n'
import CodeTab from '../panel/CodeTab'
import { useGuard } from '../panel/guard'
import LearnTab from '../panel/LearnTab'
import ParamsTab from '../panel/ParamsTab'
import ResultsTab from '../panel/ResultsTab'
import SaveBar from '../panel/SaveBar'
import type { FunctionId, PanelTab } from './types'

const TABS: { id: PanelTab; key: MessageKey }[] = [
  { id: 'results', key: 'tabResults' },
  { id: 'learn', key: 'tabLearn' },
  { id: 'params', key: 'tabParams' },
  { id: 'code', key: 'tabCode' },
]

interface Props {
  device: Device | null
  snapshot: DeviceSnapshot
  fn: FunctionId
  results: ResultsStore
}

export default function RightPanel({ device, snapshot, fn, results }: Props) {
  const { t } = useT()
  const guard = useGuard(device)
  const needConnect = needsConnection(snapshot)
  const blocked = !device || needConnect || !snapshot.supported.includes(fn) || guard.needFirmware
  // Params live in the device config; older firmware cannot read or change it.
  const paramsBlocked = blocked || lacksConfig(snapshot)
  const [tab, setTab] = useState<PanelTab>('results')
  const tabBlocked = tab === 'params' ? paramsBlocked : blocked
  const base = useId()
  const refs = useRef<Record<PanelTab, HTMLButtonElement | null>>({ results: null, learn: null, params: null, code: null })

  const selectTab = (id: PanelTab) => {
    guard.clear()
    setTab(id)
  }
  const onKeyDown = (e: KeyboardEvent, index: number) => {
    let next: number
    if (e.key === 'ArrowRight') next = (index + 1) % TABS.length
    else if (e.key === 'ArrowLeft') next = (index - 1 + TABS.length) % TABS.length
    else if (e.key === 'Home') next = 0
    else if (e.key === 'End') next = TABS.length - 1
    else return
    e.preventDefault()
    const id = TABS[next].id
    selectTab(id)
    refs.current[id]?.focus()
  }
  return (
    <aside className="rightpanel">
      <div role="tablist" className="rp-tabs">
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
            onClick={() => selectTab(id)}
          >
            {t(key)}
          </button>
        ))}
      </div>
      <div role="tabpanel" id={`${base}-panel`} aria-labelledby={`${base}-${tab}`} className="rp-tabpanel">
        {tab !== 'results' && tab !== 'code' && tabBlocked && <p className="rp-hint rp-firmware">{t(needConnect ? 'needConnect' : 'needFirmware')}</p>}
        {guard.error && (
          <p role="alert" className="rp-error">
            {t('opFailed')} <small>{guard.error}</small>
          </p>
        )}
        {tab === 'results' && <ResultsTab fn={fn} results={results} learned={snapshot.learned[fn]} />}
        {tab === 'learn' && <LearnTab device={device} fn={fn} learned={snapshot.learned[fn]} disabled={blocked} guard={guard} />}
        {tab === 'params' && <ParamsTab device={device} fn={fn} params={snapshot.params} learned={snapshot.learned[fn]} disabled={paramsBlocked} guard={guard} />}
        {tab === 'code' && <CodeTab fn={fn} />}
      </div>
      <SaveBar device={device} dirty={snapshot.dirty} disabled={needConnect || !snapshot.supported.includes(fn)} />
    </aside>
  )
}
