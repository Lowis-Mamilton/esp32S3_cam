import { useEffect, useState } from 'react'
import type { Device } from '../../device/types'
import { useT } from '../../i18n'
import { useGuard } from './guard'

interface Props {
  device: Device | null
  dirty: boolean
  /** Function not available on the device: nothing can be saved. */
  disabled: boolean
}

export default function SaveBar({ device, dirty, disabled }: Props) {
  const { t } = useT()
  const guard = useGuard(device)
  const [busy, setBusy] = useState(false)
  const { clear } = guard
  // A stale save error must not outlive the change that made it irrelevant.
  useEffect(() => {
    clear()
  }, [dirty, clear])
  const save = async () => {
    if (!device) return
    setBusy(true)
    try {
      await guard.run(() => device.save())
    } finally {
      setBusy(false)
    }
  }
  return (
    <div className="rp-savebar">
      {guard.error && (
        <p role="alert" className="rp-error">
          {t('saveFailed')} <small>{guard.error}</small>
        </p>
      )}
      {guard.needFirmware && <p className="rp-hint">{t('needFirmware')}</p>}
      <div className="rp-saverow">
        <button
          type="button"
          className="btn primary"
          disabled={!device || !dirty || busy || disabled || guard.needFirmware}
          onClick={() => void save()}
        >
          {t('save')}
        </button>
        {dirty && <span className="rp-badge">{t('unsaved')}</span>}
      </div>
    </div>
  )
}
