import { useEffect, useImperativeHandle, useRef, type Ref } from 'react'
import type { FrameSource } from '../device/types'
import { useT } from '../i18n'

export interface LiveViewHandle {
  draw(f: FrameSource): void
}

interface Props {
  ref?: Ref<LiveViewHandle>
  connected: boolean
  fps: number
  kbps: number
}

export default function LiveView({ ref, connected, fps, kbps }: Props) {
  const { t } = useT()
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const busy = useRef(false)

  useImperativeHandle(ref, () => ({
    draw(f) {
      if (f.kind === 'canvas') {
        const canvas = canvasRef.current
        const ctx = canvas?.getContext('2d')
        if (!canvas || !ctx) return
        // A previous JPEG frame may have resized the canvas; reset so no stale pixels remain.
        if (canvas.width !== 320) canvas.width = 320
        if (canvas.height !== 240) canvas.height = 240
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

  return (
    <div className="stage">
      <canvas ref={canvasRef} width={320} height={240} />
      <div className="stage-stats">
        {fps.toFixed(1)} {t('fps')} / {kbps.toFixed(1)} {t('kbps')}
      </div>
      {!connected && <div className="stage-idle">{t('liveIdle')}</div>}
    </div>
  )
}
