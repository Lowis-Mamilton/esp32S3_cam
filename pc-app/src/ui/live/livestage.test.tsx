import { act, fireEvent, render, screen } from '@testing-library/react'
import { createRef } from 'react'
import { afterEach, expect, test, vi } from 'vitest'
import { DemoLearnError } from '../../device/demo/recognize'
import { NotSupportedError, type LearnedId, type Result } from '../../device/types'
import { I18nProvider } from '../../i18n'
import LiveStage, { type LiveStageHandle } from './LiveStage'

afterEach(() => vi.restoreAllMocks())

function stubRect(w: number, h: number) {
  vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockReturnValue({
    x: 0, y: 0, left: 0, top: 0, right: w, bottom: h, width: w, height: h, toJSON: () => ({}),
  })
}

interface Over {
  fn?: 1 | 2 | 3
  canLearn?: boolean
  onLearn?: (r: { x: number; y: number; width: number; height: number }) => Promise<number>
  results?: Result[]
  learned?: LearnedId[]
  showNames?: boolean
  fps?: number
  kbps?: number | null
}
function setup(o: Over = {}) {
  const ref = createRef<LiveStageHandle>()
  const utils = render(
    <I18nProvider>
      <LiveStage
        ref={ref}
        connected
        fps={o.fps ?? 0}
        kbps={o.kbps === undefined ? 0 : o.kbps}
        fn={o.fn ?? 1}
        canLearn={o.canLearn ?? true}
        onLearn={o.onLearn ?? (async () => 1)}
        results={o.results ?? []}
        learned={o.learned ?? []}
        showNames={o.showNames ?? true}
      />
    </I18nProvider>,
  )
  return { ref, ...utils }
}

test('canvas frames reset the canvas to 320x240 and clear it before drawing', () => {
  const calls: string[] = []
  const ctx = { clearRect: () => calls.push('clear'), drawImage: () => {} }
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(ctx as unknown as CanvasRenderingContext2D)
  const { ref, container } = setup()
  const canvas = container.querySelector('canvas')!
  canvas.width = 640 // as left by a previous JPEG frame
  canvas.height = 480
  calls.length = 0
  ref.current!.draw({ kind: 'canvas', draw: () => calls.push('draw') })
  expect(canvas.width).toBe(320)
  expect(canvas.height).toBe(240)
  expect(calls).toEqual(['clear', 'draw'])
})

test('dragging on the stage learns the region in frame coordinates', async () => {
  stubRect(640, 480) // scale 2
  const onLearn = vi.fn(async () => 3)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  fireEvent.mouseDown(stage, { clientX: 20, clientY: 20 })
  fireEvent.mouseMove(window, { clientX: 100, clientY: 80 })
  await act(async () => {
    fireEvent.mouseUp(window, { clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 10, y: 10, width: 40, height: 30 })
  expect(await screen.findByText('已學習 ID3')).toBeTruthy()
})

test('drag is clamped to the frame and tiny drags are ignored', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  fireEvent.mouseDown(stage, { clientX: 600, clientY: 400 })
  await act(async () => {
    fireEvent.mouseUp(window, { clientX: 900, clientY: 700 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 300, y: 200, width: 20, height: 40 })
  onLearn.mockClear()
  fireEvent.mouseDown(stage, { clientX: 100, clientY: 100 })
  await act(async () => {
    fireEvent.mouseUp(window, { clientX: 102, clientY: 102 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('no dragging when learning is unavailable', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn, canLearn: false })
  fireEvent.mouseDown(container.querySelector('.stage')!, { clientX: 0, clientY: 0 })
  await act(async () => {
    fireEvent.mouseUp(window, { clientX: 200, clientY: 200 })
  })
  expect(onLearn).not.toHaveBeenCalled()
  expect(screen.queryByRole('button', { name: '學習中央' })).toBeNull()
})

test('learn-centre button learns a 16x16 region at the centre', async () => {
  const onLearn = vi.fn(async () => 2)
  setup({ onLearn })
  await act(async () => {
    fireEvent.click(screen.getByRole('button', { name: '學習中央' }))
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 152, y: 112, width: 16, height: 16 })
})

test('errors map to messages', async () => {
  const cases: [unknown, string][] = [
    [new DemoLearnError('no-color'), '框選範圍內沒有顏色'],
    [new DemoLearnError('full'), '最多只能學 7 個顏色'],
    [new NotSupportedError(), '需要更新韌體'],
  ]
  for (const [err, msg] of cases) {
    const { unmount } = setup({ onLearn: () => Promise.reject(err) })
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: '學習中央' }))
    })
    expect(await screen.findByText(msg)).toBeTruthy()
    unmount()
  }
})

test('boxes carry labels and ID colours; fps/KBs shown, KB/s hidden when null', () => {
  stubRect(640, 480)
  const results: Result[] = [
    { id: 1, kind: 'color', x: 30, y: 50, width: 40, height: 40 },
    { id: 0, kind: 'color', x: 100, y: 0, width: 20, height: 20 },
  ]
  const { container } = setup({ results, learned: [{ id: 1, name: '紅色積木' }], fps: 9.9, kbps: null })
  expect(screen.getByText('ID1 紅色積木')).toBeTruthy()
  expect(screen.getByText('顏色')).toBeTruthy()
  const boxes = container.querySelectorAll<HTMLElement>('.ls-box')
  expect(boxes[0].style.getPropertyValue('--c')).toBe('var(--id-1)')
  expect(boxes[0].style.left).toBe('20px')
  expect(boxes[0].style.width).toBe('80px')
  expect(screen.getByText(/9\.9 fps/)).toBeTruthy()
  expect(screen.queryByText(/KB\/s/)).toBeNull()
})

test('box is positioned from its centre x/y (frame rect 140..180 x 110..130)', () => {
  stubRect(640, 480) // scale 2
  const { container } = setup({ results: [{ id: 1, kind: 'color', x: 160, y: 120, width: 40, height: 20 }] })
  const box = container.querySelector<HTMLElement>('.ls-box')!
  expect(box.style.left).toBe('280px')
  expect(box.style.top).toBe('220px')
  expect(box.style.width).toBe('80px')
  expect(box.style.height).toBe('40px')
})

test('crosshair is shown even when learning is unavailable (real device)', () => {
  stubRect(640, 480)
  const { container } = setup({ canLearn: false })
  const ch = container.querySelector<HTMLElement>('.ls-crosshair')!
  expect(ch.style.left).toBe('320px')
  expect(ch.style.top).toBe('240px')
})

test('tabs render in a layer after all boxes so no box covers a tab', () => {
  stubRect(640, 480)
  const { container } = setup({
    results: [
      { id: 1, kind: 'color', x: 100, y: 100, width: 80, height: 80 },
      { id: 2, kind: 'color', x: 110, y: 110, width: 80, height: 80 },
    ],
  })
  const boxes = container.querySelector('.ls-boxes')!
  const tabs = container.querySelector('.ls-labels')!
  expect(boxes.querySelectorAll('.ls-box').length).toBe(2)
  expect(tabs.querySelectorAll('.ls-label').length).toBe(2)
  expect(boxes.compareDocumentPosition(tabs) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
  expect(boxes.querySelector('.ls-label')).toBeNull()
})

test('tabs stay inside the stage at the right edge and for negative box origins', () => {
  stubRect(640, 480)
  const { container } = setup({
    results: [
      { id: 1, kind: 'color', x: 310, y: 120, width: 40, height: 40 }, // right edge
      { id: 2, kind: 'color', x: 10, y: 5, width: 60, height: 40 }, // left < 0, top < 0
    ],
  })
  const tabs = [...container.querySelectorAll<HTMLElement>('.ls-label')]
  for (const tab of tabs) {
    const left = parseFloat(tab.style.left)
    const top = parseFloat(tab.style.top)
    const w = parseFloat(tab.style.width)
    expect(left).toBeGreaterThanOrEqual(0)
    expect(left + w).toBeLessThanOrEqual(640)
    expect(top).toBeGreaterThanOrEqual(0)
    expect(top + 18).toBeLessThanOrEqual(480)
  }
})

test('learn message appears in the controls row next to the learn button', async () => {
  setup({ onLearn: () => Promise.reject(new DemoLearnError('no-color')) })
  await act(async () => {
    fireEvent.click(screen.getByRole('button', { name: '學習中央' }))
  })
  const msg = await screen.findByText('框選範圍內沒有顏色')
  expect(msg.closest('.stage-actions')).not.toBeNull()
})

test('Escape cancels a drag', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  fireEvent.mouseDown(container.querySelector('.stage')!, { clientX: 20, clientY: 20 })
  fireEvent.keyDown(window, { key: 'Escape' })
  await act(async () => {
    fireEvent.mouseUp(window, { clientX: 200, clientY: 200 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})
