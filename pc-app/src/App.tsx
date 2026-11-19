import { useCallback, useEffect, useRef, useState } from 'react'
import type { Device, FunctionId, Result } from './device/types'
import { useDevice } from './device/useDevice'
import { describeError, type ErrorText } from './i18n/core'
import { useT } from './i18n'
import RightPanel from './ui/layout/RightPanel'
import Sidebar from './ui/layout/Sidebar'
import TopBar from './ui/layout/TopBar'
import type { View } from './ui/layout/types'
import LiveStage, { type LiveStageHandle } from './ui/live/LiveStage'

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
  const [errorOf, setErrorOf] = useState<{ device: Device; error: ErrorText } | null>(null)
  const [statsOf, setStatsOf] = useState<{ device: Device; fps: number; kbps: number } | null>(null)
  const error = errorOf && errorOf.device === device ? errorOf.error : null
  const stats = statsOf && statsOf.device === device ? statsOf : { fps: 0, kbps: 0 }
  const [resultsOf, setResultsOf] = useState<{ device: Device; results: Result[] } | null>(null)
  const results = resultsOf && resultsOf.device === device ? resultsOf.results : []
  const liveRef = useRef<LiveStageHandle>(null)
  const userClosed = useRef(false)

  useEffect(() => {
    if (!device) return
    const offs = [
      device.onFrame((f) => liveRef.current?.draw(f)),
      device.onResults((r) => setResultsOf({ device, results: r })),
      device.onStats((x) => setStatsOf({ device, ...x })),
    ]
    return () => offs.forEach((off) => off())
  }, [device])

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
        <Sidebar view={view} supported={supported} onSelect={select} />
        <main className="center">
          {status === 'unsupported' ? (
            <p className="notice">{t('unsupported')}</p>
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
        <RightPanel device={device} snapshot={snapshot} fn={lastFn} results={results} />
      </div>
    </div>
  )
}
