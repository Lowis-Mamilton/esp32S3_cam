import { useEffect, useRef, useState } from 'react'
import type { DeviceInfo } from './protocol/messages'
import { MxcamConnection, type ConnectionStatus } from './serial/connection'
import { strings } from './strings'
import DevicePanel from './ui/DevicePanel'
import LiveView, { type LiveViewHandle } from './ui/LiveView'

interface Props {
  /** Injectable for tests; defaults to navigator.serial. */
  serial?: Serial
}

export default function App({ serial }: Props) {
  const resolved = serial ?? (typeof navigator !== 'undefined' && 'serial' in navigator ? navigator.serial : undefined)
  const [status, setStatus] = useState<ConnectionStatus>(resolved ? 'disconnected' : 'unsupported')
  const [lost, setLost] = useState(false)
  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const [error, setError] = useState<string | null>(null)
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
    connRef.current?.connect().catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)))
  }
  const onDisconnect = () => {
    userClosed.current = true
    void connRef.current?.disconnect()
  }

  return (
    <main>
      <h1>{strings.appTitle}</h1>
      {status === 'unsupported' ? (
        <p>{strings.unsupported}</p>
      ) : (
        <div style={{ display: 'flex', gap: 16, alignItems: 'flex-start' }}>
          <LiveView ref={liveRef} fps={stats.fps} kbps={stats.kbps} />
          <DevicePanel
            status={status}
            lost={lost}
            info={info}
            error={error}
            onConnect={onConnect}
            onDisconnect={onDisconnect}
          />
        </div>
      )}
    </main>
  )
}
