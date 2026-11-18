import { useEffect, useRef, useState } from 'react'
import type { DeviceInfo } from './protocol/messages'
import { MxcamConnection, type ConnectionStatus } from './serial/connection'
import { describeError, type ErrorText } from './i18n/core'
import { useT } from './i18n'
import RightPanel from './ui/layout/RightPanel'
import Sidebar from './ui/layout/Sidebar'
import TopBar from './ui/layout/TopBar'
import type { FunctionId, View } from './ui/layout/types'
import LiveView, { type LiveViewHandle } from './ui/LiveView'

interface Props {
  /** Injectable for tests; defaults to navigator.serial. */
  serial?: Serial
}

// Task 2 derives this from the device (GET_INFO functionIds / demo mode).
const SUPPORTED: FunctionId[] = [1, 2, 3]

export default function App({ serial }: Props) {
  const { t } = useT()
  const [view, setView] = useState<View>({ kind: 'function', id: 1 })
  const resolved = serial ?? (typeof navigator !== 'undefined' && 'serial' in navigator ? navigator.serial : undefined)
  const [status, setStatus] = useState<ConnectionStatus>(resolved ? 'disconnected' : 'unsupported')
  const [lost, setLost] = useState(false)
  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const [error, setError] = useState<ErrorText | null>(null)
  const [stats, setStats] = useState({ fps: 0, kbps: 0 })
  const liveRef = useRef<LiveViewHandle>(null)
  const connRef = useRef<MxcamConnection | null>(null)
  const userClosed = useRef(false)

  useEffect(() => {
    if (!resolved) return
    const conn = new MxcamConnection(resolved, {
      status: (s) => {
        setStatus(s)
        if (s === 'connecting') {
          userClosed.current = false
          setLost(false)
          setError(null)
        } else if (s === 'disconnected') {
          setLost(!userClosed.current)
          setInfo(null)
        }
      },
      frame: (f) => liveRef.current?.draw(f),
      stats: setStats,
      info: setInfo,
    })
    connRef.current = conn
    // Deferred so StrictMode's mount/unmount/mount does not race two opens of one port.
    const t = setTimeout(() => void conn.autoConnect(), 0)
    return () => {
      clearTimeout(t)
      connRef.current = null
      void conn.dispose()
    }
  }, [resolved])

  const onConnect = () => {
    connRef.current?.connect().catch((e: unknown) => {
      if (!userClosed.current) setError(describeError(e))
    })
  }
  const onDisconnect = () => {
    userClosed.current = true
    void connRef.current?.disconnect()
  }

  return (
    <div className="app">
      <TopBar status={status} lost={lost} info={info} onConnect={onConnect} onDisconnect={onDisconnect} />
      <div className="columns">
        {error && (
          <p role="alert" className="alert">
            {t('error')}: {t(error.key)}
            {error.detail && (
              <>
                {' '}
                <small>{error.detail}</small>
              </>
            )}
          </p>
        )}
        <Sidebar view={view} supported={SUPPORTED} onSelect={setView} />
        <main className="center">
          {status === 'unsupported' ? (
            <p className="notice">{t('unsupported')}</p>
          ) : view.kind === 'function' ? (
            <LiveView ref={liveRef} connected={status === 'connected'} fps={stats.fps} kbps={stats.kbps} />
          ) : (
            <section className="page">
              {/* Task 6 builds the real settings / firmware pages. */}
              <h2>{view.kind === 'settings' ? t('pageSettings') : t('pageFirmware')}</h2>
              {view.kind === 'settings' && info && (
                <dl className="info">
                  <dt>{t('protocolVersion')}</dt>
                  <dd>{info.protocolVersion}</dd>
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
                  <dt>{t('maxResults')}</dt>
                  <dd>{info.maxResults}</dd>
                  <dt>{t('functions')}</dt>
                  <dd>{info.functionIds.join(', ')}</dd>
                </dl>
              )}
              <p className="muted">{t('pageSoon')}</p>
            </section>
          )}
        </main>
        <RightPanel />
      </div>
    </div>
  )
}
