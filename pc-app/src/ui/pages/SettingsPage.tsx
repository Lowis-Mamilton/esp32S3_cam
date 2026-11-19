import { useEffect, useRef, useState } from 'react'
import { isValidI2cAddress } from '../../device/config'
import type { Device, DeviceSnapshot } from '../../device/types'
import { useT } from '../../i18n'
import { useGuard } from '../panel/guard'
import { Slider } from '../panel/ParamsTab'

interface Props {
  device: Device | null
  snapshot: DeviceSnapshot
}

const BAUDS = [115200, 9600] as const
const HEX = /^[0-9a-f]{1,2}$/i

function parseHex(text: string): number | null {
  if (!HEX.test(text)) return null
  const n = parseInt(text, 16)
  return isValidI2cAddress(n) ? n : null
}

export default function SettingsPage({ device, snapshot }: Props) {
  const { t } = useT()
  const guard = useGuard(device)
  const { info, system } = snapshot
  const disabled = !device || guard.needFirmware
  const set = (patch: Partial<typeof system>) => (device ? guard.run(() => device.setSystem(patch)) : undefined)

  // The address is only sent on Enter or blur: a half-typed value must never reach the bus.
  const [draft, setDraft] = useState<string | null>(null)
  const [rangeError, setRangeError] = useState(false)
  const shown = guard.needFirmware ? null : draft
  const hex = shown ?? system.i2cAddress.toString(16).toUpperCase()
  const commitHex = () => {
    if (shown === null) return
    const n = parseHex(shown.trim().replace(/^0x/i, ''))
    if (n === null) {
      setRangeError(true)
      return
    }
    setDraft(null)
    setRangeError(false)
    if (n !== system.i2cAddress) void set({ i2cAddress: n })
  }
  const revertHex = () => {
    setDraft(null)
    setRangeError(false)
  }
  const hexBad = rangeError && shown !== null

  const [confirming, setConfirming] = useState(false)
  const cancelRef = useRef<HTMLButtonElement>(null)
  const resetRef = useRef<HTMLButtonElement>(null)
  const returnFocus = useRef(false)
  useEffect(() => {
    if (confirming) cancelRef.current?.focus()
    else if (returnFocus.current) {
      returnFocus.current = false
      resetRef.current?.focus()
    }
  }, [confirming])
  const closeConfirm = () => {
    returnFocus.current = true
    setConfirming(false)
  }

  return (
    <section className="pg">
      <h2>{t('pageSettings')}</h2>
      {guard.needFirmware && <p className="rp-hint rp-firmware">{t('needFirmware')}</p>}
      {guard.error && (
        <p role="alert" className="rp-error">
          {t('opFailed')} <small>{guard.error}</small>
        </p>
      )}

      <div className="pg-section">
        <h3 className="pg-h">{t('deviceInfo')}</h3>
        {info ? (
          <dl className="pg-info">
            <dt>{t('firmware')}</dt>
            <dd>
              {info.firmwareVersion} ({info.firmwareGit})
            </dd>
            <dt>{t('serialNumber')}</dt>
            <dd>{info.serial}</dd>
            <dt>{t('frameSize')}</dt>
            <dd>
              {info.frameWidth}x{info.frameHeight}
            </dd>
            <dt>{t('protocolVersion')}</dt>
            <dd>{info.protocolVersion}</dd>
          </dl>
        ) : (
          <p className="muted">{t('statusDisconnected')}</p>
        )}
      </div>

      <div className="pg-section">
        <h3 className="pg-h">{t('secComms')}</h3>
        <div className="pg-row">
          <span id="pg-baud">{t('uartBaud')}</span>
          <div className="pg-seg" role="radiogroup" aria-labelledby="pg-baud">
            {BAUDS.map((b) => (
              <label key={b} className={system.uartBaud === b ? 'on' : undefined}>
                <input type="radio" name="uartBaud" checked={system.uartBaud === b} disabled={disabled} onChange={() => void set({ uartBaud: b })} />
                {b}
              </label>
            ))}
          </div>
        </div>
        <div className="pg-row">
          <label htmlFor="pg-i2c">{t('i2cAddress')}</label>
          <div className="pg-hex">
            <span>0x</span>
            <input
              id="pg-i2c"
              type="text"
              spellCheck={false}
              autoComplete="off"
              maxLength={4}
              value={hex}
              disabled={disabled}
              aria-invalid={hexBad}
              aria-describedby={hexBad ? 'pg-i2c-err' : undefined}
              onChange={(e) => {
                setDraft(e.target.value.toUpperCase())
                setRangeError(false)
              }}
              onBlur={commitHex}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && !e.nativeEvent.isComposing && e.keyCode !== 229) commitHex()
                else if (e.key === 'Escape') revertHex()
              }}
            />
          </div>
        </div>
        {hexBad && (
          <p id="pg-i2c-err" className="rp-hint pg-err">
            {t('i2cRange')}
          </p>
        )}
      </div>

      <div className="pg-section">
        <h3 className="pg-h">{t('secLed')}</h3>
        <Slider key="led" label={t('ledBrightness')} value={system.ledBrightness} min={0} max={100} disabled={disabled} onCommit={(v) => set({ ledBrightness: v })} />
      </div>

      <div className="pg-section">
        <h3 className="pg-h">{t('factoryReset')}</h3>
        {confirming ? (
          <div className="rp-confirm" role="group" aria-label={t('factoryReset')}>
            <p>{t('factoryResetText')}</p>
            <button ref={cancelRef} type="button" className="btn" onClick={closeConfirm}>
              {t('cancel')}
            </button>
            <button
              type="button"
              className="btn rp-danger"
              onClick={() => {
                closeConfirm()
                if (device) void guard.run(() => device.factoryReset())
              }}
            >
              {t('factoryResetConfirm')}
            </button>
          </div>
        ) : (
          <>
            <p className="muted pg-note">{t('factoryResetHint')}</p>
            <button ref={resetRef} type="button" className="btn" disabled={disabled} onClick={() => setConfirming(true)}>
              {t('factoryReset')}
            </button>
          </>
        )}
      </div>
    </section>
  )
}
