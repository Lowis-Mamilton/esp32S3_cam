import { Bluetooth, Plug, Unplug } from 'lucide-react'
import type { DeviceInfo } from '../../protocol/messages'
import type { ConnectionStatus } from '../../serial/connection'
import type { TransportKind } from '../../transport/types'
import { useT } from '../../i18n'

interface Props {
  status: ConnectionStatus
  /** True when the last disconnect was not requested by the user. */
  lost: boolean
  info: DeviceInfo | null
  demo: boolean
  onToggleDemo(on: boolean): void
  /** How this browser can connect; one connect button each. Default: USB only. */
  transports?: TransportKind[]
  /** The transport being opened or in use. */
  transport?: TransportKind | null
  onConnect(kind: TransportKind): void
  onDisconnect(): void
}

const USB_ONLY: TransportKind[] = ['usb']

export default function TopBar({ status, lost, info, demo, onToggleDemo, transports = USB_ONLY, transport = null, onConnect, onDisconnect }: Props) {
  const { t, lang, setLang } = useT()
  // With a single way to connect the button just says "Connect"; with two, each names its transport.
  const connectLabel = (k: TransportKind) => (transports.length < 2 ? t('connect') : k === 'usb' ? t('connectUsb') : t('connectBluetooth'))
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
      <div className="tb-start">
        <h1 className="brand">{t('appTitle')}</h1>
        {demo && <span className="demo-badge">{t('demoBadge')}</span>}
      </div>
      {/* Centred like an editor's document name: the board this page is talking to. */}
      <div className="tb-centre">
        {(!unsupported || demo) && (
          <div className="conn-status" data-tone={tone}>
            <span className="lamp" aria-hidden="true" />
            <span>{statusText}</span>
            {busy && transport && (
              <span className="tb-via" data-transport={transport}>
                {transport === 'usb' ? t('viaUsb') : t('viaBluetooth')}
              </span>
            )}
            {info && <span className="fw-version">{info.firmwareVersion}</span>}
          </div>
        )}
      </div>
      <div className="tb-end">
        <button
          type="button"
          role="switch"
          aria-checked={demo}
          className="tb-switch"
          data-slot="demo-toggle"
          onClick={() => onToggleDemo(!demo)}
        >
          <span className="tb-switch-track" aria-hidden="true" />
          {t('demoMode')}
        </button>
        <div className="tb-seg" role="group" aria-label={t('language')}>
          <button type="button" lang="zh-Hant" aria-pressed={lang === 'zh-TW'} onClick={() => setLang('zh-TW')}>
            繁中
          </button>
          <button type="button" lang="en" aria-pressed={lang === 'en'} onClick={() => setLang('en')}>
            English
          </button>
        </div>
        {!unsupported && !demo && (
          <>
            {transports.map((k, i) => (
              <button
                key={k}
                type="button"
                className={i === 0 ? 'btn tb-btn primary' : 'btn tb-btn tb-alt'}
                data-transport={k}
                onClick={() => onConnect(k)}
                disabled={busy}
              >
                {k === 'usb' ? <Plug size={16} aria-hidden="true" /> : <Bluetooth size={16} aria-hidden="true" />}
                {connectLabel(k)}
              </button>
            ))}
            <button type="button" className="btn tb-btn" onClick={onDisconnect} disabled={!busy}>
              <Unplug size={16} aria-hidden="true" />
              {t('disconnect')}
            </button>
          </>
        )}
      </div>
    </header>
  )
}
