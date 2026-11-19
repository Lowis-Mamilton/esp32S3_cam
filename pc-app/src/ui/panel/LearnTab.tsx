import { Trash2 } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { Device, LearnedId } from '../../device/types'
import { useT } from '../../i18n'
import type { FunctionId } from '../layout/types'
import type { Guard } from './guard'

const MAX_NAME = 32
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
    if (draft !== null && draft !== value) onCommit(draft)
    setDraft(null)
  }
  return (
    <input
      className="rp-input"
      type="text"
      aria-label={label}
      placeholder={t('unnamed')}
      maxLength={MAX_NAME}
      value={draft ?? value}
      disabled={disabled}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => e.key === 'Enter' && !e.nativeEvent.isComposing && e.keyCode !== 229 && commit()}
    />
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
          <input className="rp-input" type="text" maxLength={MAX_NAME} value={name} disabled={disabled} onChange={(e) => setName(e.target.value)} />
        </label>
        <button type="submit" className="btn" disabled={disabled || !valid}>
          {t('add')}
        </button>
      </form>
      {tag !== '' && !(Number.isInteger(n) && n >= 0 && n <= MAX_TAG) && <p className="rp-hint">{t('tagRange', { max: MAX_TAG })}</p>}
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

export default function LearnTab({ fn, ...rest }: Props) {
  const { t } = useT()
  if (fn === 3) return <p className="muted">{t('faceNoLearn')}</p>
  return fn === 1 ? <ColorLearn {...rest} /> : <TagLearn {...rest} />
}
