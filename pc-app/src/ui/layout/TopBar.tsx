import { Plug, Unplug } from 'lucide-react'
import type { DeviceInfo } from '../../protocol/messages'
import type { ConnectionStatus } from '../../serial/connection'
import { useT } from '../../i18n'

interface Props {
  status: ConnectionStatus
  /** True when the last disconnect was not requested by the user. */
  lost: boolean
  info: DeviceInfo | null
  onConnect(): void
  onDisconnect(): void
}

export default function TopBar({ status, lost, info, onConnect, onDisconnect }: Props) {
  const { t, lang, setLang } = useT()
  const busy = status === 'connecting' || status === 'connected'
  const unsupported = status === 'unsupported'
  const statusText =
    status === 'connecting'
      ? t('statusConnecting')
      : status === 'connected'
        ? t('statusConnected')
        : lost
          ? t('statusLost')
          : t('statusDisconnected')
  const tone = status === 'connected' ? 'on' : status === 'connecting' ? 'wait' : lost ? 'lost' : 'off'

  return (
    <header className="topbar">
      <h1 className="brand">{t('appTitle')}</h1>
      {!unsupported && (
        <div className="conn-status" data-tone={tone}>
          <span className="lamp" aria-hidden="true" />
          <span>{statusText}</span>
          {info && <span className="fw-version">{info.firmwareVersion}</span>}
        </div>
      )}
      <div className="topbar-spacer" />
      {/* Task 2 replaces this placeholder with the real demo-mode toggle. */}
      <button type="button" role="switch" aria-checked="false" className="switch" disabled data-slot="demo-toggle">
        <span className="switch-track" aria-hidden="true" />
        {t('demoMode')}
      </button>
      <div className="seg" role="group" aria-label={t('language')}>
        <button type="button" aria-pressed={lang === 'zh-TW'} onClick={() => setLang('zh-TW')}>
          繁中
        </button>
        <button type="button" aria-pressed={lang === 'en'} onClick={() => setLang('en')}>
          English
        </button>
      </div>
      {!unsupported && (
        <>
          <button type="button" className="btn primary" onClick={onConnect} disabled={busy}>
            <Plug size={16} aria-hidden="true" />
            {t('connect')}
          </button>
          <button type="button" className="btn" onClick={onDisconnect} disabled={!busy}>
            <Unplug size={16} aria-hidden="true" />
            {t('disconnect')}
          </button>
        </>
      )}
    </header>
  )
}
