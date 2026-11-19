import { render } from '@testing-library/react'
import { createRef } from 'react'
import { expect, test, vi } from 'vitest'
import { I18nProvider } from '../i18n'
import LiveView, { type LiveViewHandle } from './LiveView'

test('canvas frames reset the canvas to 320x240 and clear it before drawing', () => {
  const calls: string[] = []
  const ctx = { clearRect: () => calls.push('clear'), drawImage: () => {} }
  const spy = vi
    .spyOn(HTMLCanvasElement.prototype, 'getContext')
    .mockReturnValue(ctx as unknown as CanvasRenderingContext2D)
  const ref = createRef<LiveViewHandle>()
  const { container } = render(
    <I18nProvider>
      <LiveView ref={ref} connected fps={0} kbps={0} />
    </I18nProvider>,
  )
  const canvas = container.querySelector('canvas')!
  canvas.width = 640 // as left by a previous JPEG frame
  canvas.height = 480
  calls.length = 0
  ref.current!.draw({ kind: 'canvas', draw: () => calls.push('draw') })
  expect(canvas.width).toBe(320)
  expect(canvas.height).toBe(240)
  expect(calls).toEqual(['clear', 'draw'])
  spy.mockRestore()
})
