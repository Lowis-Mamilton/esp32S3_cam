import { useRef, useState } from 'react'
import type { DeviceSnapshot } from '../../device/types'
import { useT } from '../../i18n'

/** Flat steel-blue drawing of the board's chip. Decorative. */
function ChipArt() {
  const pins = [0, 1, 2, 3, 4]
  return (
    <svg className="pg-art-svg" viewBox="0 0 200 120" aria-hidden="true">
      {pins.map((i) => (
        <g key={i} fill="var(--art-mid)">
          <rect x={64 + i * 16} y={14} width={7} height={14} rx={2} />
          <rect x={64 + i * 16} y={92} width={7} height={14} rx={2} />
        </g>
      ))}
      {[0, 1, 2].map((i) => (
        <g key={i} fill="var(--art-mid)">
          <rect x={40} y={42 + i * 14} width={14} height={7} rx={2} />
          <rect x={146} y={42 + i * 14} width={14} height={7} rx={2} />
        </g>
      ))}
      <rect x={52} y={26} width={96} height={68} rx={8} fill="var(--art-deep)" />
      <rect x={66} y={38} width={68} height={44} rx={4} fill="none" stroke="var(--art-mid)" strokeWidth={3} />
      <circle cx={100} cy={60} r={9} fill="var(--art-light)" />
    </svg>
  )
}

/** A firmware file going onto the board. Decorative. */
function UploadArt() {
  return (
    <svg className="pg-art-svg" viewBox="0 0 200 120" aria-hidden="true">
      <path d="M62 14h48l22 22v70H62z" fill="var(--art-light)" />
      <path d="M110 14v22h22" fill="var(--art-mid)" />
      {[52, 64, 76].map((y) => (
        <rect key={y} x={74} y={y} width={44} height={6} rx={3} fill="var(--art-mid)" />
      ))}
      <circle cx={140} cy={84} r={24} fill="var(--art-deep)" />
      <path d="M140 72v24M129 83l11-11 11 11" fill="none" stroke="#fff" strokeWidth={5} strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  )
}

/** UI only for now: choosing a file works, flashing is not implemented. */
export default function FirmwarePage({ snapshot }: { snapshot: DeviceSnapshot }) {
  const { t } = useT()
  const input = useRef<HTMLInputElement>(null)
  const [fileName, setFileName] = useState<string | null>(null)
  const [soon, setSoon] = useState(false)
  const info = snapshot.info
  return (
    <section className="pg pg-wide">
      <h2>{t('pageFirmware')}</h2>
      <div className="pg-tiles">
        <div className="pg-tile">
          <div className="pg-art">
            <ChipArt />
          </div>
          <div className="pg-tile-body">
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
        </div>
        <div className="pg-tile">
          <div className="pg-art">
            <UploadArt />
          </div>
          <div className="pg-tile-body">
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
        </div>
      </div>
    </section>
  )
}
