import { X } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ResultsStore } from './device/resultsStore'
import { needsConnection, type Device, type FunctionId } from './device/types'
import { useDevice } from './device/useDevice'
import { describeError, type ErrorText } from './i18n/core'
import { useT, type MessageKey } from './i18n'
import RightPanel from './ui/layout/RightPanel'
import Sidebar from './ui/layout/Sidebar'
import TopBar from './ui/layout/TopBar'
import type { View } from './ui/layout/types'
import FirmwarePage from './ui/pages/FirmwarePage'
import SettingsPage from './ui/pages/SettingsPage'
import LiveStage, { type LiveStageHandle } from './ui/live/LiveStage'

const FN_KEY: Record<FunctionId, MessageKey> = { 1: 'fnColor', 2: 'fnTag', 3: 'fnFace' }
const NOTICE_MS = 4000

interface Props {
  /** Injectable for tests; defaults to navigator.serial. */
  serial?: Serial
}

export default function App({ serial }: Props) {
  const { t } = useT()
  const [view, setView] = useState<View>({ kind: 'function', id: 1 })
  // The right panel keeps showing the last chosen function while a settings page is open.
  const [lastFn, setLastFn] = useState<FunctionId>(1)
  const select = (v: View) => {
    setView(v)
    if (v.kind === 'function') setLastFn(v.id)
  }
  const resolved = serial ?? (typeof navigator !== 'undefined' && 'serial' in navigator ? navigator.serial : undefined)
  const { device, snapshot, demo, setDemo } = useDevice(resolved)
  const { status, lost, info, supported } = snapshot
  // Errors and stats belong to one device instance; ignore them once it is replaced.
  const [errorOf, setErrorOf] = useState<{
    device: Device
    error: ErrorText
  } | null>(null)
  const [statsOf, setStatsOf] = useState<{
    device: Device
    fps: number
    kbps: number
  } | null>(null)
  const error = errorOf && errorOf.device === device ? errorOf.error : null
  const stats = statsOf && statsOf.device === device ? statsOf : { fps: 0, kbps: 0 }
  // Results live outside React state: only LiveStage and the results tab subscribe (push rate is ~10/s).
  // One store per device instance, so a new device never shows the previous one's boxes.
  // oxlint-disable-next-line react-hooks/exhaustive-deps -- device is the store's identity key
  const results = useMemo(() => new ResultsStore(), [device])
  // "Switched by another device" notice; belongs to one device instance.
  const [noticeOf, setNoticeOf] = useState<{ device: Device; fn: FunctionId } | null>(null)
  const notice = noticeOf && noticeOf.device === device ? noticeOf.fn : null
  const liveRef = useRef<LiveStageHandle>(null)
  const userClosed = useRef(false)

  useEffect(() => {
    if (!device) return
    const offs = [
      device.onFrame((f) => liveRef.current?.draw(f)),
      device.onResults((r, meta) => results.push(r, meta?.frameId)),
      device.onStats((x) => setStatsOf({ device, ...x })),
      // Another host (e.g. the R4) changed the function: follow it so the view matches what the board runs.
      device.onFunctionChange((fn) => {
        setLastFn(fn)
        setView((v) => (v.kind === 'function' ? { kind: 'function', id: fn } : v))
        setNoticeOf({ device, fn })
      }),
    ]
    return () => offs.forEach((off) => off())
  }, [device, results])

  useEffect(() => {
    if (!noticeOf) return
    const timer = setTimeout(() => setNoticeOf(null), NOTICE_MS)
    return () => clearTimeout(timer)
  }, [noticeOf])

  // The device must be told which function is active: it does not know what the user is looking at.
  // Sent on every view change and after each (re)connect; older firmware rejects it, which is fine.
  const connected = status === 'connected'
  useEffect(() => {
    if (!connected) results.clear()
  }, [connected, results])
  useEffect(() => {
    if (!device || !connected) return
    device.setFunction(lastFn).catch(() => {})
  }, [device, connected, lastFn])

  const onConnect = () => {
    if (!device) return
    userClosed.current = false
    setErrorOf(null)
    device.connect().catch((e: unknown) => {
      if (!userClosed.current) setErrorOf({ device, error: describeError(e) })
    })
  }
  const onLearn = useCallback((region: Parameters<Device['learnColor']>[0]) => device!.learnColor(region), [device])
  const onDisconnect = () => {
    userClosed.current = true
    void device?.disconnect()
  }

  return (
    <div className="app">
      <TopBar status={status} lost={lost} info={info ?? null} demo={demo} onToggleDemo={setDemo} onConnect={onConnect} onDisconnect={onDisconnect} />
      {error && (
        <p role="alert" className="app-alert">
          <span>
            {t('error')}: {t(error.key)}
            {error.detail && (
              <>
                {' '}
                <small>{error.detail}</small>
              </>
            )}
          </span>
          <button type="button" className="app-alert-close" aria-label={t('dismiss')} onClick={() => setErrorOf(null)}>
            <X size={16} aria-hidden="true" />
          </button>
        </p>
      )}
      {notice && (
        <p role="status" className="app-info">
          {t('fnSwitched', { fn: t(FN_KEY[notice]) })}
        </p>
      )}
      <div className="app-columns">
        <Sidebar view={view} supported={supported} needConnect={needsConnection(snapshot)} onSelect={select} />
        <main className="app-center">
          {status === 'unsupported' ? (
            <p className="app-notice">{t('unsupported')}</p>
          ) : view.kind === 'function' ? (
            <LiveStage
              ref={liveRef}
              connected={status === 'connected'}
              fps={stats.fps}
              kbps={demo ? null : stats.kbps}
              fn={view.id}
              canLearn={view.id === 1 && !!device && (demo || supported.includes(1))}
              onLearn={onLearn}
              results={results}
              learned={snapshot.learned[view.id]}
              showNames={view.id === 3 ? false : snapshot.params[view.id === 1 ? 'color' : 'tag'].showNames}
            />
          ) : view.kind === 'settings' ? (
            <SettingsPage device={device} snapshot={snapshot} />
          ) : (
            <FirmwarePage snapshot={snapshot} />
          )}
        </main>
        <RightPanel device={device} snapshot={snapshot} fn={lastFn} results={results} />
      </div>
    </div>
  )
}
