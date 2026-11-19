import { useEffect, useId, useRef, useState } from 'react'
import { ConfigFileError } from '../../device/config'
import { NotSupportedError, type Device, type FunctionParams } from '../../device/types'
import { useT, type MessageKey } from '../../i18n'
import type { FunctionId } from '../layout/types'
import type { Guard } from './guard'

const DEBOUNCE_MS = 150

interface SliderProps {
  label: string
  value: number
  /** Replaces the number next to the label (e.g. a dash when the value is unknown). */
  valueText?: string
  min: number
  max: number
  step?: number
  disabled: boolean
  onCommit(v: number): Promise<unknown> | void
  /** Small note under the slider, tied to it with aria-describedby. */
  hint?: string
}

/** Shows the value live while dragging; sends it to the device once the user pauses. */
export function Slider({ label, value, valueText, min, max, step = 1, disabled, onCommit, hint }: SliderProps) {
  const hintId = useId()
  const [draft, setDraft] = useState<number | null>(null)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const pending = useRef<{ v: number; commit: SliderProps['onCommit'] } | null>(null)
  // Leaving the tab must not drop a change that is still waiting out the debounce.
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current)
      const p = pending.current
      pending.current = null
      if (p) void p.commit(p.v)
    },
    [],
  )
  const shown = draft ?? value
  const change = (v: number) => {
    setDraft(v)
    pending.current = { v, commit: onCommit }
    if (timer.current) clearTimeout(timer.current)
    timer.current = setTimeout(() => {
      timer.current = null
      pending.current = null
      void Promise.resolve(onCommit(v)).finally(() => setDraft(null))
    }, DEBOUNCE_MS)
  }
  return (
    <div className="rp-slider">
      <div className="rp-slider-head">
        <span>{label}</span>
        <output>{draft === null && valueText ? valueText : shown}</output>
      </div>
      <input type="range" aria-label={label} min={min} max={max} step={step} value={shown} disabled={disabled} aria-describedby={hint ? hintId : undefined} onChange={(e) => change(Number(e.target.value))} />
      <div className="rp-slider-range" aria-hidden="true">
        <span>{min}</span>
        <span>{max}</span>
      </div>
      {hint && (
        <p id={hintId} className="rp-hint">
          {hint}
        </p>
      )}
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

const isNotSupported = (e: unknown) => e instanceof NotSupportedError || (e instanceof Error && e.name === 'NotSupportedError')

function readText(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const r = new FileReader()
    r.onload = () => resolve(String(r.result))
    r.onerror = () => reject(r.error ?? new Error('read failed'))
    r.readAsText(file)
  })
}

const MAX_CONFIG_BYTES = 1024 * 1024
const REVOKE_DELAY_MS = 1000

type ImportState =
  | { kind: 'ok'; relearn: boolean }
  | { kind: 'bad'; key?: MessageKey; raw?: string }
  /** The device refused part of a valid file; the reason is shown by the guard. */
  | { kind: 'partial' }
  | null
const isFileError = (e: unknown) => e instanceof ConfigFileError || (e instanceof Error && e.name === 'ConfigFileError')

/** Export / import of the whole device config (all functions + system). */
function ConfigIO({ device, disabled, guard }: { device: Device | null; disabled: boolean; guard: Guard }) {
  const { t } = useT()
  const input = useRef<HTMLInputElement>(null)
  const [state, setState] = useState<ImportState>(null)
  const exportFile = () => {
    if (!device) return
    // Goes through the guard: a device without config support throws NotSupportedError here.
    void guard.run(async () => {
      const url = URL.createObjectURL(new Blob([device.exportConfig()], { type: 'application/json' }))
      const a = document.createElement('a')
      a.href = url
      a.download = 'mxcam-config.json'
      document.body.appendChild(a)
      a.click()
      a.remove()
      // Revoking at once can cancel the download in some browsers.
      setTimeout(() => URL.revokeObjectURL(url), REVOKE_DELAY_MS)
    })
  }
  const importFile = async (file: File) => {
    if (!device) return
    setState(null)
    guard.clear()
    if (file.size > MAX_CONFIG_BYTES) {
      setState({ kind: 'bad', key: 'cfgTooLarge' })
      return
    }
    try {
      const out = await device.importConfig(await readText(file))
      setState({ kind: 'ok', relearn: out.colorsNeedRelearn })
    } catch (e) {
      if (isFileError(e)) {
        const msg = e instanceof Error ? e.message : String(e)
        setState(/not JSON/.test(msg) ? { kind: 'bad', key: 'cfgNotJson' } : { kind: 'bad', raw: msg })
        return
      }
      // Not a bad file: the device refused or failed (names full, busy, timeout, disconnected, old firmware).
      void guard.run(() => Promise.reject(e))
      if (!isNotSupported(e)) setState({ kind: 'partial' })
    }
  }
  return (
    <div className="rp-config">
      <h3 className="rp-h">{t('cfgHeading')}</h3>
      <div className="rp-actions">
        <button type="button" className="btn" disabled={!device || disabled} onClick={exportFile}>
          {t('cfgExport')}
        </button>
        <button type="button" className="btn" disabled={disabled} onClick={() => input.current?.click()}>
          {t('cfgImport')}
        </button>
        <input
          ref={input}
          type="file"
          accept="application/json,.json"
          hidden
          aria-label={t('cfgFileInput')}
          onChange={(e) => {
            const f = e.target.files?.[0]
            e.target.value = ''
            if (f) void importFile(f)
          }}
        />
      </div>
      {state?.kind === 'ok' && (
        <p role="status" className="rp-hint">
          {t('cfgImported')} <small>{t('cfgImportedHint')}</small>
        </p>
      )}
      {state?.kind === 'ok' && state.relearn && <p className="rp-hint">{t('cfgRelearn')}</p>}
      {state?.kind === 'partial' && <p className="rp-hint">{t('cfgPartial')}</p>}
      {state?.kind === 'bad' && (
        <p role="alert" className="rp-error">
          {t('cfgBad')} <small>{state.key ? t(state.key) : state.raw}</small>
        </p>
      )}
    </div>
  )
}

function FnParams({ device, fn, params, disabled, guard }: Props) {
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
        <Slider key="1-tolerance" label={t('paramTolerance')} value={c.tolerance} min={0} max={100} disabled={disabled} onCommit={(v) => color({ tolerance: v })} />
        <Slider key="1-minArea" label={t('paramMinArea')} value={c.minArea} min={0} max={5000} step={10} disabled={disabled} onCommit={(v) => color({ minArea: v })} />
        <Slider key="1-maxResults" label={t('paramMaxResults')} value={c.maxResults} min={1} max={8} disabled={disabled} onCommit={(v) => color({ maxResults: v })} />
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
  if (fn === 8) {
    const g = params.gesture
    const gesture = (patch: Partial<FunctionParams['gesture']>) => set((p) => ({ gesture: { ...p.gesture, ...patch } }))
    return (
      <div className="rp-params">
        <Slider key="8-hand" label={t('paramHandThreshold')} value={g.handThreshold} min={10} max={95} disabled={disabled} onCommit={(v) => gesture({ handThreshold: v })} />
        <Slider key="8-gesture" label={t('paramGestureThreshold')} value={g.gestureThreshold} min={0} max={100} disabled={disabled} onCommit={(v) => gesture({ gestureThreshold: v })} />
        <div className="rp-field" role="group" aria-label={t('paramMaxHands')}>
          <span>{t('paramMaxHands')}</span>
          <div className="rp-actions">
            {([1, 2] as const).map((n) => (
              <button key={n} type="button" className="btn" aria-pressed={g.maxHands === n} disabled={disabled} onClick={() => void gesture({ maxHands: n })}>
                {n}
              </button>
            ))}
          </div>
          <p className="rp-hint">{t('maxHandsHint')}</p>
        </div>
      </div>
    )
  }
  const f = params.face
  const face = (patch: Partial<FunctionParams['face']>) => set((p) => ({ face: { ...p.face, ...patch } }))
  return (
    <div className="rp-params">
      <Slider key="3-threshold" label={t('paramThreshold')} value={f.threshold} min={0} max={100} disabled={disabled} hint={t('faceThresholdHint')} onCommit={(v) => face({ threshold: v })} />
      <Slider key="3-maxResults" label={t('paramMaxResults')} value={f.maxResults} min={1} max={8} disabled={disabled} onCommit={(v) => face({ maxResults: v })} />
    </div>
  )
}

export default function ParamsTab(props: Props) {
  return (
    <>
      <FnParams {...props} />
      <ConfigIO device={props.device} disabled={props.disabled} guard={props.guard} />
    </>
  )
}
