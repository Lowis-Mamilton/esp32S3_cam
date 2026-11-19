import { useEffect, useRef, useState } from 'react'
import type { Device, FunctionParams } from '../../device/types'
import { useT, type MessageKey } from '../../i18n'
import type { FunctionId } from '../layout/types'
import type { Guard } from './guard'

const DEBOUNCE_MS = 150

interface SliderProps {
  label: string
  value: number
  min: number
  max: number
  step?: number
  disabled: boolean
  onCommit(v: number): Promise<unknown> | void
}

/** Shows the value live while dragging; sends it to the device once the user pauses. */
function Slider({ label, value, min, max, step = 1, disabled, onCommit }: SliderProps) {
  const [draft, setDraft] = useState<number | null>(null)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current)
    },
    [],
  )
  const shown = draft ?? value
  const change = (v: number) => {
    setDraft(v)
    if (timer.current) clearTimeout(timer.current)
    timer.current = setTimeout(() => {
      timer.current = null
      void Promise.resolve(onCommit(v)).finally(() => setDraft(null))
    }, DEBOUNCE_MS)
  }
  return (
    <div className="rp-slider">
      <div className="rp-slider-head">
        <span>{label}</span>
        <output>{shown}</output>
      </div>
      <input type="range" aria-label={label} min={min} max={max} step={step} value={shown} disabled={disabled} onChange={(e) => change(Number(e.target.value))} />
      <div className="rp-slider-range" aria-hidden="true">
        <span>{min}</span>
        <span>{max}</span>
      </div>
    </div>
  )
}

interface Props {
  device: Device | null
  fn: FunctionId
  params: FunctionParams
  disabled: boolean
  guard: Guard
}

export default function ParamsTab({ device, fn, params, disabled, guard }: Props) {
  const { t } = useT()
  const set = (patch: (p: FunctionParams) => Partial<FunctionParams>) =>
    device ? guard.run(() => device.setParams(patch(device.snapshot().params))) : undefined
  const checkbox = (checked: boolean, on: (v: boolean) => void, key: MessageKey) => (
    <label className="rp-check">
      <input type="checkbox" checked={checked} disabled={disabled} onChange={(e) => on(e.target.checked)} />
      {t(key)}
    </label>
  )

  if (fn === 1) {
    const c = params.color
    const color = (patch: Partial<FunctionParams['color']>) => set((p) => ({ color: { ...p.color, ...patch } }))
    return (
      <div className="rp-params">
        <Slider label={t('paramTolerance')} value={c.tolerance} min={0} max={100} disabled={disabled} onCommit={(v) => color({ tolerance: v })} />
        <Slider label={t('paramMinArea')} value={c.minArea} min={0} max={5000} step={10} disabled={disabled} onCommit={(v) => color({ minArea: v })} />
        <Slider label={t('paramMaxResults')} value={c.maxResults} min={1} max={8} disabled={disabled} onCommit={(v) => color({ maxResults: v })} />
        {checkbox(c.showNames, (v) => void color({ showNames: v }), 'paramShowNames')}
      </div>
    )
  }
  if (fn === 2) {
    return (
      <div className="rp-params">
        <div className="rp-field">
          <span>{t('paramTagFamily')}</span>
          <code className="rp-readonly">tag36h11</code>
        </div>
        {checkbox(params.tag.showNames, (v) => void set((p) => ({ tag: { ...p.tag, showNames: v } })), 'paramShowNames')}
      </div>
    )
  }
  const f = params.face
  const face = (patch: Partial<FunctionParams['face']>) => set((p) => ({ face: { ...p.face, ...patch } }))
  return (
    <div className="rp-params">
      <Slider label={t('paramThreshold')} value={f.threshold} min={0} max={100} disabled={disabled} onCommit={(v) => face({ threshold: v })} />
      <Slider label={t('paramMaxResults')} value={f.maxResults} min={1} max={8} disabled={disabled} onCommit={(v) => face({ maxResults: v })} />
    </div>
  )
}
