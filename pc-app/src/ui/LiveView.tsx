import { useImperativeHandle, useRef, type Ref } from 'react'
import type { FramePush } from '../protocol/messages'
import { strings } from '../strings'

export interface LiveViewHandle {
  draw(f: FramePush): void
}

interface Props {
  ref?: Ref<LiveViewHandle>
  fps: number
  kbps: number
}

export default function LiveView({ ref, fps, kbps }: Props) {
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
          if (canvas.width !== f.width) canvas.width = f.width
          if (canvas.height !== f.height) canvas.height = f.height
          canvas.getContext('2d')?.drawImage(bmp, 0, 0)
          bmp.close()
        })
        .catch(() => {})
        .finally(() => {
          busy.current = false
        })
    },
  }))

  return (
    <div style={{ position: 'relative', width: 640, height: 480, background: '#000' }}>
      <canvas ref={canvasRef} width={320} height={240} style={{ width: 640, height: 480 }} />
      <div style={{ position: 'absolute', left: 8, top: 8, color: '#0f0', font: '14px monospace' }}>
        {fps.toFixed(1)} {strings.fps} / {kbps.toFixed(1)} {strings.kbps}
      </div>
    </div>
  )
}
