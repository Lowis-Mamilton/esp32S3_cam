import { Crosshair } from 'lucide-react'
import { useCallback, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, useSyncExternalStore, type CSSProperties, type MouseEvent as ReactMouseEvent, type Ref } from 'react'
import type { ResultsStore } from '../../device/resultsStore'
import { LearnError, NotConnectedError, NotSupportedError, type FrameSource, type LearnedId } from '../../device/types'
import { useT } from '../../i18n'
import CategoryDisc from '../layout/CategoryDisc'
import { FN_CATEGORY, FN_ICON } from '../layout/categories'
import type { FunctionId } from '../layout/types'
import { useMirror } from './mirror'
import { FRAME_H, FRAME_W, boxToScreen, mirrorRegion, toFrame, toScreen, type Viewport } from './geometry'
import { DRAG_HINT_KEY, FN_KIND, LEARNED_KEY, LEARN_ERROR_KEY, STAGE_KEY, idColorVar, labelFor } from './labels'

export interface LiveStageHandle {
  draw(f: FrameSource): void
}

type Region = { x: number; y: number; width: number; height: number }

interface Props {
  ref?: Ref<LiveStageHandle>
  connected: boolean
  fps: number
  /** null hides the KB/s figure (demo mode has no real link). */
  kbps: number | null
  fn: FunctionId
  /** A learning function (colour, line colour, object) and a device that can learn. */
  canLearn: boolean
  onLearn: (region: Region) => Promise<number>
  /** Only this component and the results tab subscribe, so pushes do not re-render the whole app. */
  results: ResultsStore
  /** Learned IDs of the displayed function. */
  learned: LearnedId[]
  showNames: boolean
  /** Line tracking: the share of the picture (from the bottom) the device looks at, drawn as a dashed edge. */
  lineRoi?: number
  /** Frames are not sent; results keep coming (the device layer owns the state). */
  previewPaused?: boolean
  onPausePreview?: (paused: boolean) => void
}

const MIN_DRAG = 4 // frame px
const TAB_H = 18
/** Rough tab width: CJK glyphs are about twice as wide as Latin ones at 12px. */
const tabWidth = (text: string) => Math.ceil([...text].reduce((w, c) => w + (c.charCodeAt(0) > 255 ? 13 : 7), 14))
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v))
const CENTRE: Region = { x: 152, y: 112, width: 16, height: 16 }

export default function LiveStage({ ref, connected, fps, kbps, fn, canLearn, onLearn, results: store, learned, showNames, lineRoi = 60, previewPaused = false, onPausePreview }: Props) {
  const { t } = useT()
  const results = useSyncExternalStore(store.subscribe, store.paired)
  const stageRef = useRef<HTMLDivElement>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const busy = useRef(false)
  const [vp, setVp] = useState<Viewport>({ width: 0, height: 0 })
  const [drag, setDrag] = useState<{ x0: number; y0: number; x1: number; y1: number } | null>(null)
  const [toast, setToast] = useState<{ text: string; error: boolean } | null>(null)
  // Results and LEARN regions are TRUE-image coordinates; only the screen is mirrored.
  const [mirror, setMirror] = useMirror()
  // A layout effect runs synchronously in the commit, so a decode finishing right after the pause cannot slip through.
  const pausedRef = useRef(previewPaused)
  useLayoutEffect(() => {
    pausedRef.current = previewPaused
  }, [previewPaused])
  const [resultsRate, setResultsRate] = useState(0)
  const dragCleanup = useRef<(() => void) | null>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)

  useImperativeHandle(ref, () => ({
    draw(f) {
      if (pausedRef.current) return // a frame that was already in flight when the preview was paused
      if (f.kind === 'canvas') {
        const canvas = canvasRef.current
        const ctx = canvas?.getContext('2d')
        if (!canvas || !ctx) return
        // A previous JPEG frame may have resized the canvas; reset so no stale pixels remain.
        if (canvas.width !== FRAME_W) canvas.width = FRAME_W
        if (canvas.height !== FRAME_H) canvas.height = FRAME_H
        ctx.clearRect(0, 0, canvas.width, canvas.height)
        f.draw(ctx)
        store.setDisplayed(undefined)
        return
      }
      // No createImageBitmap (e.g. jsdom): nothing to decode; treat the frame as shown.
      if (typeof createImageBitmap !== 'function') {
        store.setDisplayed(f.frameId)
        return
      }
      // Drop the frame if the previous decode is still running; never queue.
      if (busy.current) return
      const canvas = canvasRef.current
      if (!canvas) return
      busy.current = true
      createImageBitmap(new Blob([f.jpeg as BlobPart], { type: 'image/jpeg' }))
        .then((bmp) => {
          try {
            if (pausedRef.current) return // paused while this JPEG was decoding: no picture, no frame id
            if (canvas.width !== bmp.width) canvas.width = bmp.width
            if (canvas.height !== bmp.height) canvas.height = bmp.height
            canvas.getContext('2d')?.drawImage(bmp, 0, 0)
            // Boxes follow the picture: results are paired by frame id (review point 4).
            store.setDisplayed(f.frameId)
          } finally {
            bmp.close()
          }
        })
        .catch(() => {})
        .finally(() => {
          busy.current = false
        })
    },
  }))

  // Paused: no frame is on screen, so boxes follow the newest results; the stats show the results rate.
  useEffect(() => {
    if (!previewPaused) return
    try {
      const canvas = canvasRef.current
      canvas?.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height)
    } catch {
      // ignore
    }
    store.setDisplayed(undefined)
    let count = 0
    let since = Date.now()
    const off = store.subscribe(() => count++)
    const timer = setInterval(() => {
      const now = Date.now()
      setResultsRate((count * 1000) / Math.max(1, now - since))
      count = 0
      since = now
    }, 1000)
    return () => {
      off()
      clearInterval(timer)
      setResultsRate(0)
    }
  }, [previewPaused, store])

  useEffect(() => {
    if (connected) return
    const canvas = canvasRef.current
    // jsdom has no canvas context; getContext may be missing or return null
    try {
      canvas?.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height)
    } catch {
      // ignore
    }
  }, [connected])

  // Track the displayed size; the overlay is laid out in CSS pixels so text and lines stay crisp.
  useLayoutEffect(() => {
    const el = stageRef.current
    const canvas = canvasRef.current
    if (!el || !canvas) return
    const measure = () => {
      // The canvas fills the stage's content box, so its rect has no border skew.
      const r = canvas.getBoundingClientRect()
      setVp((p) => (p.width === r.width && p.height === r.height ? p : { width: r.width, height: r.height }))
    }
    measure()
    if (typeof ResizeObserver === 'undefined') return
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  useEffect(() => () => clearTimeout(toastTimer.current), [])
  // Abort any drag in flight on unmount or when learning becomes unavailable.
  useEffect(() => {
    if (!canLearn) dragCleanup.current?.()
    return () => dragCleanup.current?.()
  }, [canLearn])

  const say = useCallback((text: string, error: boolean) => {
    setToast({ text, error })
    clearTimeout(toastTimer.current)
    toastTimer.current = setTimeout(() => setToast(null), 3000)
  }, [])

  const learn = useCallback(
    async (region: Region) => {
      try {
        const id = await onLearn(region)
        say(t(LEARNED_KEY[fn] ?? 'learned', { id }), false)
      } catch (e) {
        if (e instanceof NotSupportedError) say(t('learnNeedFirmware'), true)
        else if (e instanceof NotConnectedError) say(t('needConnect'), true)
        else if (e instanceof LearnError) say(t(LEARN_ERROR_KEY[e.code]), true)
        else say(t('learnFailed'), true)
      }
    },
    [fn, onLearn, say, t],
  )

  const frameAt = (clientX: number, clientY: number) => {
    const r = canvasRef.current!.getBoundingClientRect()
    const f = toFrame({ width: r.width, height: r.height }, clientX - r.left, clientY - r.top, mirror)
    return { x: clamp(f.x, 0, FRAME_W), y: clamp(f.y, 0, FRAME_H) }
  }

  const onMouseDown = (e: ReactMouseEvent) => {
    if (!canLearn || e.button !== 0) return
    e.preventDefault()
    const a = frameAt(e.clientX, e.clientY)
    let cur = { x0: a.x, y0: a.y, x1: a.x, y1: a.y }
    setDrag(cur)
    const move = (ev: MouseEvent) => {
      const b = frameAt(ev.clientX, ev.clientY)
      cur = { ...cur, x1: b.x, y1: b.y }
      setDrag(cur)
    }
    const cleanup = () => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      window.removeEventListener('keydown', key)
      dragCleanup.current = null
      setDrag(null)
    }
    const key = (ev: KeyboardEvent) => {
      if (ev.key === 'Escape') cleanup()
    }
    const up = (ev: MouseEvent) => {
      cleanup()
      const b = frameAt(ev.clientX, ev.clientY)
      const x = Math.round(Math.min(a.x, b.x))
      const y = Math.round(Math.min(a.y, b.y))
      const width = Math.round(Math.max(a.x, b.x)) - x
      const height = Math.round(Math.max(a.y, b.y)) - y
      if (width < MIN_DRAG || height < MIN_DRAG) return
      void learn({ x, y, width, height })
    }
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
    window.addEventListener('keydown', key)
    dragCleanup.current = cleanup
  }

  const shown = results.filter((r) => r.kind === FN_KIND[fn])
  const lineRows = shown.filter((r) => r.line)
  const roiY = toScreen(vp, 0, FRAME_H - (FRAME_H * lineRoi) / 100, mirror).py
  const lost = fn === 5 && connected && learned.length > 0 && shown.length === 0
  const centre = toScreen(vp, FRAME_W / 2, FRAME_H / 2, mirror)
  let dragBox: { left: number; top: number; width: number; height: number } | null = null
  if (drag) {
    const w = Math.abs(drag.x1 - drag.x0)
    const h = Math.abs(drag.y1 - drag.y0)
    const box = { x: Math.min(drag.x0, drag.x1) + w / 2, y: Math.min(drag.y0, drag.y1) + h / 2, width: w, height: h }
    dragBox = boxToScreen(vp, box, mirror)
  }

  return (
    <div className="stage-wrap">
      {/* A hat block on top of the picture: what the board is running right now. */}
      <div className="ls-hat">
        <CategoryDisc cat={FN_CATEGORY[fn]} Icon={FN_ICON[fn]} size={15} />
        <span>{t(STAGE_KEY[fn])}</span>
      </div>
      <div ref={stageRef} className={`stage${canLearn ? ' learnable' : ''}`} onMouseDown={onMouseDown}>
        <canvas ref={canvasRef} width={FRAME_W} height={FRAME_H} className={mirror ? 'mirrored' : undefined} />
        {previewPaused && connected && <div className="stage-paused">{t('previewPausedNote')}</div>}
        <div className="ls-overlay" aria-hidden="true">
          <div className="ls-boxes">
            {shown.filter((r) => r.kind !== 'line').map((r, i) => {
              // Result x/y are the box centre, in true-image coordinates.
              const style = { '--c': idColorVar(r), ...boxToScreen(vp, r, mirror) } as CSSProperties
              return <div key={i} className={`ls-box${(r.kind === 'color' || r.kind === 'gesture') && r.id === 0 ? ' unlearned' : ''}`} style={style} />
            })}
          </div>
          {fn === 4 && lineRows.length > 0 && (
            <svg className="ls-line" width={vp.width} height={vp.height} viewBox={`0 0 ${vp.width} ${vp.height}`}>
              <line className="ls-roi" x1={0} x2={vp.width} y1={roiY} y2={roiY} />
              {lineRows.map((r, i) => {
                const a = toScreen(vp, r.line!.start[0], r.line!.start[1], mirror)
                const b = toScreen(vp, r.line!.end[0], r.line!.end[1], mirror)
                return (
                  <g key={i}>
                    <line className="ls-seg" x1={a.px} y1={a.py} x2={b.px} y2={b.py} />
                    <circle className="ls-end" cx={a.px} cy={a.py} r={5} />
                  </g>
                )
              })}
            </svg>
          )}
          <div className="ls-labels">
            {shown.map((r, i) => {
              const box = boxToScreen(vp, r, mirror)
              const bottom = box.top + box.height
              // The line gets its flags as extra badges beside the main tab.
              const tabs = [labelFor(r, learned, showNames, t)]
              if (r.line?.cross) tabs.push(t('lineCross'))
              if (r.line?.branch) tabs.push(t('lineBranch'))
              let top = box.top - TAB_H - 1 // above the box
              if (top < 0) top = bottom + 1 // below, if there is room
              if (top + TAB_H > vp.height) top = box.top + 1 // otherwise inside the box top
              top = clamp(top, 0, Math.max(0, vp.height - TAB_H))
              let left = clamp(box.left - 2, 0, Math.max(0, vp.width - tabs.map(tabWidth).reduce((a, w) => a + w + 2, -2)))
              return tabs.map((text, j) => {
                const w = tabWidth(text)
                const style = { '--c': idColorVar(r), left, top, width: w } as CSSProperties
                left += w + 2
                return (
                  <span key={`${i}-${j}`} className={`ls-label${j > 0 ? ' ls-badge' : ''}`} style={style}>
                    {text}
                  </span>
                )
              })
            })}
          </div>
          {lost && <div className="ls-lost">{t('targetLost')}</div>}
          {connected && !previewPaused && <div className="ls-crosshair" style={{ left: centre.px, top: centre.py }} />}
          {dragBox && <div className="ls-drag" style={dragBox} />}
        </div>
        <div className="stage-stats">
          {previewPaused ? (
            <>
              {t('resultsRate')} {resultsRate.toFixed(1)}/s
            </>
          ) : (
            <>
              {fps.toFixed(1)} {t('fps')}
            </>
          )}
          {!previewPaused && kbps !== null && (
            <>
              {' / '}
              {kbps.toFixed(1)} {t('kbps')}
            </>
          )}
        </div>
        {!connected && <div className="stage-idle">{t('liveIdle')}</div>}
      </div>
      <div className="stage-actions">
        {canLearn && (
          <>
            {/* The centre square in screen space, converted to the true image. */}
            <button type="button" className="btn ls-block" onClick={() => void learn(mirror ? mirrorRegion(CENTRE) : CENTRE)}>
              <Crosshair className="ls-block-icon" size={18} aria-hidden="true" />
              <span className="ls-block-label">{t('learnCenter')}</span>
            </button>
            {toast ? (
              <span role="status" className={`ls-msg${toast.error ? ' error' : ''}`}>
                {toast.text}
              </span>
            ) : (
              <span className="muted">{t(DRAG_HINT_KEY[fn] ?? 'dragHint')}</span>
            )}
          </>
        )}
        <label className="ls-mirror ls-pause">
          <input type="checkbox" checked={previewPaused} disabled={!connected} onChange={(e) => onPausePreview?.(e.target.checked)} />
          {t('pausePreview')}
        </label>
        <label className="ls-mirror">
          <input type="checkbox" checked={mirror} onChange={(e) => setMirror(e.target.checked)} />
          {t('mirrorView')}
        </label>
      </div>
    </div>
  )
}
