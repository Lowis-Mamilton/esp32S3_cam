import { useCallback, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, type CSSProperties, type MouseEvent as ReactMouseEvent, type Ref } from 'react'
import { DemoLearnError } from '../../device/demo/recognize'
import { NotSupportedError, type FrameSource, type LearnedId, type Result } from '../../device/types'
import { useT } from '../../i18n'
import type { FunctionId } from '../layout/types'
import { FRAME_H, FRAME_W, toFrame, toScreen, type Viewport } from './geometry'
import { idColorVar, labelFor } from './labels'

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
  /** Colour function and a device that can learn. */
  canLearn: boolean
  onLearn: (region: Region) => Promise<number>
  results: Result[]
  /** Learned IDs of the displayed function. */
  learned: LearnedId[]
  showNames: boolean
}

const MIN_DRAG = 4 // frame px
const TAB_H = 18
/** Rough tab width: CJK glyphs are about twice as wide as Latin ones at 12px. */
const tabWidth = (text: string) => Math.ceil([...text].reduce((w, c) => w + (c.charCodeAt(0) > 255 ? 13 : 7), 14))
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v))

export default function LiveStage({ ref, connected, fps, kbps, fn, canLearn, onLearn, results, learned, showNames }: Props) {
  const { t } = useT()
  const stageRef = useRef<HTMLDivElement>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const busy = useRef(false)
  const [vp, setVp] = useState<Viewport>({ width: 0, height: 0 })
  const [drag, setDrag] = useState<{ x0: number; y0: number; x1: number; y1: number } | null>(null)
  const [toast, setToast] = useState<{ text: string; error: boolean } | null>(null)
  const dragCleanup = useRef<(() => void) | null>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)

  useImperativeHandle(ref, () => ({
    draw(f) {
      if (f.kind === 'canvas') {
        const canvas = canvasRef.current
        const ctx = canvas?.getContext('2d')
        if (!canvas || !ctx) return
        // A previous JPEG frame may have resized the canvas; reset so no stale pixels remain.
        if (canvas.width !== FRAME_W) canvas.width = FRAME_W
        if (canvas.height !== FRAME_H) canvas.height = FRAME_H
        ctx.clearRect(0, 0, canvas.width, canvas.height)
        f.draw(ctx)
        return
      }
      // No createImageBitmap (e.g. jsdom): nothing to draw.
      if (typeof createImageBitmap !== 'function') return
      // Drop the frame if the previous decode is still running; never queue.
      if (busy.current) return
      const canvas = canvasRef.current
      if (!canvas) return
      busy.current = true
      createImageBitmap(new Blob([f.jpeg as BlobPart], { type: 'image/jpeg' }))
        .then((bmp) => {
          try {
            if (canvas.width !== bmp.width) canvas.width = bmp.width
            if (canvas.height !== bmp.height) canvas.height = bmp.height
            canvas.getContext('2d')?.drawImage(bmp, 0, 0)
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
        say(t('learned', { id }), false)
      } catch (e) {
        if (e instanceof NotSupportedError) say(t('learnNeedFirmware'), true)
        else if (e instanceof DemoLearnError) say(t(e.code === 'full' ? 'learnFull' : 'learnNoColor'), true)
        else say(t('learnFailed'), true)
      }
    },
    [onLearn, say, t],
  )

  const frameAt = (clientX: number, clientY: number) => {
    const r = canvasRef.current!.getBoundingClientRect()
    const f = toFrame({ width: r.width, height: r.height }, clientX - r.left, clientY - r.top)
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

  const kind = fn === 1 ? 'color' : fn === 2 ? 'tag' : 'face'
  const shown = results.filter((r) => r.kind === kind)
  const centre = toScreen(vp, FRAME_W / 2, FRAME_H / 2)
  let dragBox: { left: number; top: number; width: number; height: number } | null = null
  if (drag) {
    const a = toScreen(vp, Math.min(drag.x0, drag.x1), Math.min(drag.y0, drag.y1))
    const b = toScreen(vp, Math.max(drag.x0, drag.x1), Math.max(drag.y0, drag.y1))
    dragBox = { left: a.px, top: a.py, width: b.px - a.px, height: b.py - a.py }
  }

  return (
    <div className="stage-wrap">
      <div ref={stageRef} className={`stage${canLearn ? ' learnable' : ''}`} onMouseDown={onMouseDown}>
        <canvas ref={canvasRef} width={FRAME_W} height={FRAME_H} />
        <div className="overlay" aria-hidden="true">
          <div className="boxes">
            {shown.map((r, i) => {
              // Result x/y are the box centre.
              const a = toScreen(vp, r.x - r.width / 2, r.y - r.height / 2)
              const b = toScreen(vp, r.x + r.width / 2, r.y + r.height / 2)
              const style = {
                '--c': idColorVar(r),
                left: a.px,
                top: a.py,
                width: b.px - a.px,
                height: b.py - a.py,
              } as CSSProperties
              return <div key={i} className={`box${r.kind === 'color' && r.id === 0 ? ' unlearned' : ''}`} style={style} />
            })}
          </div>
          <div className="tabs">
            {shown.map((r, i) => {
              const a = toScreen(vp, r.x - r.width / 2, r.y - r.height / 2)
              const bottom = toScreen(vp, r.x, r.y + r.height / 2).py
              const text = labelFor(r, learned, showNames, t)
              const w = tabWidth(text)
              const left = clamp(a.px - 2, 0, Math.max(0, vp.width - w))
              let top = a.py - TAB_H - 1 // above the box
              if (top < 0) top = bottom + 1 // below, if there is room
              if (top + TAB_H > vp.height) top = a.py + 1 // otherwise inside the box top
              top = clamp(top, 0, Math.max(0, vp.height - TAB_H))
              const style = { '--c': idColorVar(r), left, top, width: w } as CSSProperties
              return (
                <span key={i} className="tab" style={style}>
                  {text}
                </span>
              )
            })}
          </div>
          <div className="crosshair" style={{ left: centre.px, top: centre.py }} />
          {dragBox && <div className="drag-rect" style={dragBox} />}
        </div>
        <div className="stage-stats">
          {fps.toFixed(1)} {t('fps')}
          {kbps !== null && (
            <>
              {' / '}
              {kbps.toFixed(1)} {t('kbps')}
            </>
          )}
        </div>
        {!connected && <div className="stage-idle">{t('liveIdle')}</div>}
      </div>
      {canLearn && (
        <div className="stage-actions">
          <button type="button" className="btn" onClick={() => void learn({ x: 152, y: 112, width: 16, height: 16 })}>
            {t('learnCenter')}
          </button>
          {toast ? (
            <span role="status" className={`toast${toast.error ? ' error' : ''}`}>
              {toast.text}
            </span>
          ) : (
            <span className="muted">{t('dragHint')}</span>
          )}
        </div>
      )}
    </div>
  )
}
