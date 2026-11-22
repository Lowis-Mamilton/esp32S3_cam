import { X } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ResultsStore } from './device/resultsStore'
import {
  needsConnection,
  NotSupportedError,
  type Device,
  type DeviceSnapshot,
  type FunctionId,
  type LearnableFn,
  type TransportKind,
} from './device/types'
import { useDevice } from './device/useDevice'
import { describeError, type ErrorText } from './i18n/core'
import { useT, type MessageKey } from './i18n'
import { browserBluetooth, type BluetoothLike } from './transport/webBluetooth'
import RightPanel from './ui/layout/RightPanel'
import Sidebar from './ui/layout/Sidebar'
import TopBar from './ui/layout/TopBar'
import type { View } from './ui/layout/types'
import FirmwarePage from './ui/pages/FirmwarePage'
import SettingsPage from './ui/pages/SettingsPage'
import LiveStage, { type LiveStageHandle } from './ui/live/LiveStage'

const FN_KEY: Record<FunctionId, MessageKey> = { 1: 'fnColor', 2: 'fnTag', 3: 'fnFace', 4: 'fnLine', 5: 'fnTrack', 8: 'fnGesture' }
/** Whether names are shown on the stage; a Record so a new function must decide (no silent fall-through). */
const SHOW_NAMES: Record<FunctionId, (p: DeviceSnapshot['params']) => boolean> = {
  1: (p) => p.color.showNames,
  2: (p) => p.tag.showNames,
  3: () => false,
  4: () => false,
  5: () => true,
  8: () => true,
}
const CAN_LEARN: Record<FunctionId, boolean> = { 1: true, 2: false, 3: false, 4: true, 5: true, 8: false }
const NOTICE_MS = 4000
/** What a tap on a greyed-out function says; the same text a mouse sees as the hover hint. */
type TapHint = { key: MessageKey }
const SET_FUNCTION_RETRY_MS = 500

interface Props {
  /** Injectable for tests; defaults to navigator.serial. */
  serial?: Serial
  /** Injectable for tests; defaults to navigator.bluetooth. */
  bluetooth?: BluetoothLike
}

export default function App({ serial, bluetooth }: Props) {
  const { t } = useT()
  const [view, setView] = useState<View>({ kind: 'function', id: 1 })
  // The right panel keeps showing the last chosen function while a settings page is open.
  const [lastFn, setLastFn] = useState<FunctionId>(1)
  const select = (v: View) => {
    setView(v)
    if (v.kind === 'function') setLastFn(v.id)
  }
  const resolved = serial ?? (typeof navigator !== 'undefined' && 'serial' in navigator ? navigator.serial : undefined)
  const { device, snapshot, demo, setDemo } = useDevice(resolved, bluetooth ?? browserBluetooth())
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
  // fn 0 = another device switched the board to "none" (the page stays as it is).
  const [noticeOf, setNoticeOf] = useState<{ device: Device; fn: FunctionId | 0 } | null>(null)
  const notice = noticeOf && noticeOf.device === device ? noticeOf.fn : null
  // A new object per tap, so tapping again restarts the timer.
  const [tapHint, setTapHint] = useState<TapHint | null>(null)
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
        if (fn !== 0) {
          setLastFn(fn)
          setView((v) => (v.kind === 'function' ? { kind: 'function', id: fn } : v))
        }
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

  useEffect(() => {
    if (!tapHint) return
    const timer = setTimeout(() => setTapHint(null), NOTICE_MS)
    return () => clearTimeout(timer)
  }, [tapHint])

  // The device must be told which function is active: it does not know what the user is looking at.
  // Sent on every view change and after each (re)connect; older firmware rejects it, which is fine.
  const connected = status === 'connected'
  useEffect(() => {
    if (!connected) results.clear()
  }, [connected, results])
  // Skipped when the board already runs that function (e.g. after following the R4), and sent at most once per
  // function and connection so an unexpected reply can never cause a request loop.
  const active = snapshot.activeFunction
  const sentFor = useRef<{ device: Device; fn: FunctionId } | null>(null)
  useEffect(() => {
    if (!device || !connected) {
      sentFor.current = null
      return
    }
    const already = sentFor.current?.device === device && sentFor.current.fn === lastFn
    sentFor.current = { device, fn: lastFn }
    if (active === lastFn || already) return
    const fn = lastFn
    const fail = (e: unknown) => setErrorOf({ device, error: { key: 'errFnSwitch', detail: describeError(e).detail ?? (e as Error)?.message } })
    device.setFunction(fn).catch((e: unknown) => {
      if (e instanceof NotSupportedError) return // older firmware: nothing to tell the user
      // One retry (a busy or briefly stalled board), then say so: recognition would run the wrong function.
      setTimeout(() => {
        if (sentFor.current?.device !== device || sentFor.current.fn !== fn) return // superseded or disconnected
        device.setFunction(fn).catch((e2: unknown) => {
          if (!(e2 instanceof NotSupportedError)) fail(e2)
        })
      }, SET_FUNCTION_RETRY_MS)
    })
  }, [device, connected, lastFn, active])

  const onConnect = (kind: TransportKind) => {
    if (!device) return
    userClosed.current = false
    setErrorOf(null)
    device.connect(kind).catch((e: unknown) => {
      if (!userClosed.current) setErrorOf({ device, error: describeError(e, kind) })
    })
  }
  const learnFn: LearnableFn = lastFn === 4 || lastFn === 5 ? lastFn : 1
  const onLearn = useCallback((region: Parameters<Device['learn']>[1]) => device!.learn(learnFn, region), [device, learnFn])
  const onPausePreview = useCallback(
    (paused: boolean) => {
      device?.setPreviewPaused(paused).catch((e: unknown) => {
        if (device) setErrorOf({ device, error: describeError(e) })
      })
    },
    [device],
  )
  const onDisconnect = () => {
    userClosed.current = true
    void device?.disconnect()
  }

  return (
    <div className="app">
      <TopBar
        status={status}
        lost={lost}
        info={info ?? null}
        demo={demo}
        onToggleDemo={setDemo}
        transports={snapshot.transports}
        transport={snapshot.transport}
        onConnect={onConnect}
        onDisconnect={onDisconnect}
      />
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
      {/* Bluetooth cannot reconnect on its own (the browser wants a click each time): say what to do. */}
      {lost && snapshot.transport === 'bluetooth' && (
        <p role="status" className="app-info">
          {t('errBtLost')}
        </p>
      )}
      {notice !== null && (
        <p role="status" className="app-info">
          {notice === 0 ? t('fnSwitchedNone') : t('fnSwitched', { fn: t(FN_KEY[notice]) })}
        </p>
      )}
      {/* Touch has no hover: a tap on a greyed-out function says why here, where the other short notices go. */}
      {tapHint && (
        <p role="status" className="app-info" data-slot="fn-hint">
          {t(tapHint.key)}
        </p>
      )}
      <div className="app-columns">
        <Sidebar
          view={view}
          supported={supported}
          needConnect={needsConnection(snapshot)}
          onSelect={(v) => {
            setTapHint(null)
            select(v)
          }}
          onUnavailable={(key) => setTapHint({ key })}
        />
        <main className="app-center">
          {status === 'unsupported' ? (
            // Neither Web Serial nor Web Bluetooth (e.g. every iPhone/iPad browser): demo mode is all that works.
            <div className="app-notice">
              <p>{t('unsupported')}</p>
              <button type="button" className="btn primary" onClick={() => setDemo(true)}>
                {t('demoTurnOn')}
              </button>
            </div>
          ) : view.kind === 'function' ? (
            <LiveStage
              ref={liveRef}
              connected={status === 'connected'}
              fps={stats.fps}
              kbps={demo ? null : stats.kbps}
              fn={view.id}
              canLearn={CAN_LEARN[view.id] && !!device && (demo || supported.includes(view.id))}
              lineRoi={snapshot.params.line.roiPercent}
              onLearn={onLearn}
              results={results}
              learned={snapshot.learned[view.id]}
              showNames={SHOW_NAMES[view.id](snapshot.params)}
              previewPaused={snapshot.previewPaused}
              onPausePreview={onPausePreview}
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
