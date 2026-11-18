import { useEffect, useImperativeHandle, useRef, type Ref } from 'react'
import type { FramePush } from '../protocol/messages'
import { strings } from '../strings'

export interface LiveViewHandle {
  draw(f: FramePush): void
}

interface Props {
  ref?: Ref<LiveViewHandle>
  connected: boolean
  fps: number
  kbps: number
}

export default function LiveView({ ref, connected, fps, kbps }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const busy = useRef(false)

  useImperativeHandle(ref, () => ({
    draw(f) {
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
            if (canvas.width !== f.width) canvas.width = f.width
            if (canvas.height !== f.height) canvas.height = f.height
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
    <div style={{ position: 'relative', width: 640, height: 480, background: '#000' }}>
      <canvas ref={canvasRef} width={320} height={240} style={{ width: 640, height: 480 }} />
      <div style={{ position: 'absolute', left: 8, top: 8, color: '#0f0', font: '14px monospace' }}>
        {fps.toFixed(1)} {strings.fps} / {kbps.toFixed(1)} {strings.kbps}
      </div>
    </div>
  )
}
