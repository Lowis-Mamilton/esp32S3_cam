import { Trash2 } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { NAME_MAX_BYTES, nameBytes, truncateBytes, type Device, type LearnedId } from '../../device/types'
import { useT, type MessageKey } from '../../i18n'
import type { FunctionId } from '../layout/types'
import type { Guard } from './guard'

/** Names are limited in UTF-8 bytes (48); no 48-byte name has more than 48 characters. */
const MAX_NAME_CHARS = NAME_MAX_BYTES
/** Keeps a name within the byte limit; IME composition is left alone until it ends. */
const limitName = (v: string, composing: boolean) => (composing ? v : truncateBytes(v))
const atLimit = (v: string) => nameBytes(v) >= NAME_MAX_BYTES
const MAX_TAG = 586

interface Props {
  device: Device | null
  fn: FunctionId
  learned: LearnedId[]
  disabled: boolean
  guard: Guard
}

function NameInput({ value, label, disabled, onCommit }: { value: string; label: string; disabled: boolean; onCommit(v: string): void }) {
  const { t } = useT()
  const [draft, setDraft] = useState<string | null>(null)
  const commit = () => {
    const v = draft === null ? null : truncateBytes(draft)
    if (v !== null && v !== value) onCommit(v)
    setDraft(null)
  }
  return (
    <>
      <input
        className="rp-input"
        type="text"
        aria-label={label}
        placeholder={t('unnamed')}
        title={t('nameLimit')}
        maxLength={MAX_NAME_CHARS}
        value={draft ?? value}
        disabled={disabled}
        onChange={(e) => setDraft(limitName(e.target.value, (e.nativeEvent as InputEvent).isComposing === true))}
        onCompositionEnd={(e) => setDraft(truncateBytes(e.currentTarget.value))}
        onBlur={commit}
        onKeyDown={(e) => e.key === 'Enter' && !e.nativeEvent.isComposing && e.keyCode !== 229 && commit()}
      />
      {draft !== null && atLimit(draft) && <span className="rp-hint rp-namehint">{t('nameLimit')}</span>}
    </>
  )
}

function ColorLearn({ device, learned, disabled, guard }: Omit<Props, 'fn'>) {
  const { t } = useT()
  const [confirming, setConfirming] = useState(false)
  const cancelRef = useRef<HTMLButtonElement>(null)
  useEffect(() => {
    if (confirming) cancelRef.current?.focus()
  }, [confirming])
  return (
    <>
      {learned.length === 0 ? (
        <p className="muted">{t('learnEmpty')}</p>
      ) : (
        <ul className="rp-list">
          {learned.map((l) => (
            <li key={l.id}>
              <span className="rp-swatch" style={{ background: l.color ? `rgb(${l.color.join(',')})` : 'var(--unlearned)' }} aria-hidden="true" />
              <span className="rp-id">ID{l.id}</span>
              <NameInput
                value={l.name}
                label={t('nameOf', { label: `ID${l.id}` })}
                disabled={disabled}
                onCommit={(v) => device && void guard.run(() => device.setName(1, l.id, v))}
              />
              <button
                type="button"
                className="rp-icon"
                aria-label={t('deleteOf', { label: `ID${l.id}` })}
                disabled={disabled}
                onClick={() => device && void guard.run(() => device.forget(1, l.id))}
              >
                <Trash2 size={15} aria-hidden="true" />
              </button>
            </li>
          ))}
        </ul>
      )}
      {
        confirming && learned.length > 0 ? (
          <div className="rp-confirm" role="group" aria-label={t('forgetAll')}>
            <p>{t('forgetConfirmText')}</p>
            <button ref={cancelRef} type="button" className="btn" onClick={() => setConfirming(false)}>
              {t('cancel')}
            </button>
            <button
              type="button"
              className="btn rp-danger"
              onClick={() => {
                setConfirming(false)
                if (device) void guard.run(() => device.forgetAll(1))
              }}
            >
              {t('forgetConfirm')}
            </button>
          </div>
        ) : (
          <button type="button" className="btn rp-forget" disabled={disabled || learned.length === 0} onClick={() => setConfirming(true)}>
            {t('forgetAll')}
          </button>
        )
      }
    </>
  )
}

function TagLearn({ device, learned, disabled, guard }: Omit<Props, 'fn'>) {
  const { t } = useT()
  const [tag, setTag] = useState('')
  const [name, setName] = useState('')
  const n = Number(tag)
  const valid = name.trim() !== '' && tag !== '' && Number.isInteger(n) && n >= 0 && n <= MAX_TAG
  const add = () => {
    if (!device || !valid) return
    void guard.run(() => device.setName(2, n, name)).then((ok) => {
      if (ok) {
        setTag('')
        setName('')
      }
    })
  }
  return (
    <>
      <h3 className="rp-h">{t('tagNameHeading')}</h3>
      <form
        className="rp-tagform"
        onSubmit={(e) => {
          e.preventDefault()
          add()
        }}
      >
        <label>
          <span>{t('tagNumber')}</span>
          <input className="rp-input" type="number" min={0} max={MAX_TAG} step={1} value={tag} disabled={disabled} onChange={(e) => setTag(e.target.value)} />
        </label>
        <label>
          <span>{t('nameLabel')}</span>
          <input
            className="rp-input"
            type="text"
            title={t('nameLimit')}
            maxLength={MAX_NAME_CHARS}
            value={name}
            disabled={disabled}
            onChange={(e) => setName(limitName(e.target.value, (e.nativeEvent as InputEvent).isComposing === true))}
            onCompositionEnd={(e) => setName(truncateBytes(e.currentTarget.value))}
          />
        </label>
        <button type="submit" className="btn" disabled={disabled || !valid}>
          {t('add')}
        </button>
      </form>
      {tag !== '' && !(Number.isInteger(n) && n >= 0 && n <= MAX_TAG) && <p className="rp-hint">{t('tagRange', { max: MAX_TAG })}</p>}
      {atLimit(name) && <p className="rp-hint">{t('nameLimit')}</p>}
      {learned.length === 0 ? (
        <p className="muted">{t('tagListEmpty')}</p>
      ) : (
        <ul className="rp-list">
          {learned.map((l) => (
            <li key={l.id}>
              <span className="rp-id">Tag {l.id}</span>
              <span className="rp-tagname">{l.name}</span>
              <button
                type="button"
                className="rp-icon"
                aria-label={t('deleteOf', { label: `Tag ${l.id}` })}
                disabled={disabled}
                onClick={() => device && void guard.run(() => device.forget(2, l.id))}
              >
                <Trash2 size={15} aria-hidden="true" />
              </button>
            </li>
          ))}
        </ul>
      )}
    </>
  )
}

const swatch = (l?: LearnedId) => (
  <span className="rp-swatch" style={{ background: l?.color ? `rgb(${l.color.join(',')})` : 'var(--unlearned)' }} aria-hidden="true" />
)

/** Black and white tape use the automatic modes; only coloured tape needs its colour learned (drag on the picture). */
function LineLearn({ device, learned, disabled, guard }: Omit<Props, 'fn'>) {
  const { t } = useT()
  const l = learned[0]
  return (
    <>
      <p className="muted">{t('lineNoLearn')}</p>
      <h3 className="rp-h">{t('lineColorHeading')}</h3>
      {l ? (
        <ul className="rp-list">
          <li>
            {swatch(l)}
            <span className="rp-id">{t('lineColorHeading')}</span>
          </li>
        </ul>
      ) : (
        <p className="muted">{t('lineNoColorYet')}</p>
      )}
      <button type="button" className="btn rp-forget" disabled={disabled || !l} onClick={() => device && void guard.run(() => device.forget(4, 1))}>
        {t('forgetToAuto')}
      </button>
    </>
  )
}

/** One tracked object: drag a box on the picture to learn it, then name it or forget it. */
function TrackLearn({ device, learned, disabled, guard }: Omit<Props, 'fn'>) {
  const { t } = useT()
  const o = learned[0]
  return (
    <>
      <p className="muted">{t('trackLearnNote')}</p>
      <h3 className="rp-h">{t('trackObjectHeading')}</h3>
      {o ? (
        <ul className="rp-list">
          <li>
            {swatch(o)}
            <NameInput value={o.name} label={t('nameOf', { label: t('catObject') })} disabled={disabled} onCommit={(v) => device && void guard.run(() => device.setName(5, 1, v))} />
          </li>
        </ul>
      ) : (
        <p className="muted">{t('trackNoneYet')}</p>
      )}
      <button type="button" className="btn rp-forget" disabled={disabled || !o} onClick={() => device && void guard.run(() => device.forget(5, 1))}>
        {t('forgetObject')}
      </button>
    </>
  )
}

const GESTURE_IDS = [1, 2, 3, 4, 5, 6, 7, 8, 9] as const

/** Gestures are fixed classes: nothing to learn, but each can be given a name (like AprilTag IDs). */
function GestureNames({ device, learned, disabled, guard }: Omit<Props, 'fn'>) {
  const { t } = useT()
  return (
    <>
      <p className="muted">{t('gestureNoLearn')}</p>
      <h3 className="rp-h">{t('gestureNamesHeading')}</h3>
      <ul className="rp-list">
        {GESTURE_IDS.map((id) => {
          const name = learned.find((l) => l.id === id)?.name ?? ''
          const label = t(`gesture${id}` as MessageKey)
          return (
            <li key={id}>
              <span className="rp-id">{label}</span>
              <NameInput value={name} label={t('nameOf', { label })} disabled={disabled} onCommit={(v) => device && void guard.run(() => device.setName(8, id, v))} />
              <button
                type="button"
                className="rp-icon"
                aria-label={t('deleteOf', { label })}
                disabled={disabled || name === ''}
                onClick={() => device && void guard.run(() => device.forget(8, id))}
              >
                <Trash2 size={15} aria-hidden="true" />
              </button>
            </li>
          )
        })}
      </ul>
    </>
  )
}

export default function LearnTab({ fn, ...rest }: Props) {
  const { t } = useT()
  if (fn === 3) return <p className="muted">{t('faceNoLearn')}</p>
  if (fn === 4) return <LineLearn {...rest} />
  if (fn === 5) return <TrackLearn {...rest} />
  if (fn === 8) return <GestureNames {...rest} />
  return fn === 1 ? <ColorLearn {...rest} /> : <TagLearn {...rest} />
}
