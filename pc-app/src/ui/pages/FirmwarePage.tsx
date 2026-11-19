import { useRef, useState } from 'react'
import type { DeviceSnapshot } from '../../device/types'
import { useT } from '../../i18n'

/** UI only for now: choosing a file works, flashing is not implemented. */
export default function FirmwarePage({ snapshot }: { snapshot: DeviceSnapshot }) {
  const { t } = useT()
  const input = useRef<HTMLInputElement>(null)
  const [fileName, setFileName] = useState<string | null>(null)
  const [soon, setSoon] = useState(false)
  const info = snapshot.info
  return (
    <section className="pg">
      <h2>{t('pageFirmware')}</h2>
      <div className="pg-section">
        <h3 className="pg-h">{t('fwCurrent')}</h3>
        <p className="pg-version">
          {info ? (
            <>
              <strong>{info.firmwareVersion}</strong> <small>({info.firmwareGit})</small>
            </>
          ) : (
            t('statusDisconnected')
          )}
        </p>
      </div>
      <div className="pg-section">
        <input
          ref={input}
          type="file"
          accept=".bin,application/octet-stream"
          hidden
          aria-label={t('fwChoose')}
          onChange={(e) => {
            setFileName(e.target.files?.[0]?.name ?? null)
            setSoon(false)
            e.target.value = ''
          }}
        />
        <div className="pg-actions">
          <button type="button" className="btn" onClick={() => input.current?.click()}>
            {t('fwChoose')}
          </button>
          <span className={fileName ? 'pg-file' : 'pg-file muted'}>{fileName ?? t('fwNoFile')}</span>
        </div>
        <div className="pg-actions">
          <button type="button" className="btn primary" onClick={() => setSoon(true)}>
            {t('fwStart')}
          </button>
          {soon && (
            <p role="status" className="pg-soon">
              {t('comingSoon')}
            </p>
          )}
        </div>
      </div>
    </section>
  )
}
