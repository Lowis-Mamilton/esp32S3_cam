import { act, fireEvent, render, screen } from '@testing-library/react'
import { createRef } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoLearnError } from '../../device/demo/recognize'
import { ResultsStore } from '../../device/resultsStore'
import { LearnError, NotConnectedError, NotSupportedError, type LearnedId, type Result } from '../../device/types'
import { I18nProvider } from '../../i18n'
import LiveStage, { type LiveStageHandle } from './LiveStage'

afterEach(() => vi.restoreAllMocks())
// The geometry tests below were written for the unmirrored view; the mirror tests at the end clear this.
beforeEach(() => localStorage.setItem('mxcam.mirror', '0'))

function stubRect(w: number, h: number) {
  vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockReturnValue({
    x: 0, y: 0, left: 0, top: 0, right: w, bottom: h, width: w, height: h, toJSON: () => ({}),
  })
}

interface Over {
  connected?: boolean
  fn?: 1 | 2 | 3 | 4 | 5 | 8
  lineRoi?: number
  canLearn?: boolean
  onLearn?: (r: { x: number; y: number; width: number; height: number }) => Promise<number>
  results?: Result[] | ResultsStore
  learned?: LearnedId[]
  showNames?: boolean
  fps?: number
  kbps?: number | null
  previewPaused?: boolean
  onPausePreview?: (paused: boolean) => void
}
function storeOf(results: Result[]) {
  const s = new ResultsStore()
  s.push(results)
  return s
}
function setup(o: Over = {}) {
  const ref = createRef<LiveStageHandle>()
  const utils = render(
    <I18nProvider>
      <LiveStage
        ref={ref}
        connected={o.connected ?? true}
        fps={o.fps ?? 0}
        kbps={o.kbps === undefined ? 0 : o.kbps}
        fn={o.fn ?? 1}
        canLearn={o.canLearn ?? true}
        onLearn={o.onLearn ?? (async () => 1)}
        results={o.results instanceof ResultsStore ? o.results : storeOf(o.results ?? [])}
        learned={o.learned ?? []}
        showNames={o.showNames ?? true}
        lineRoi={o.lineRoi}
        previewPaused={o.previewPaused}
        onPausePreview={o.onPausePreview}
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
  fireEvent.pointerDown(stage, { clientX: 20, clientY: 20 })
  fireEvent.pointerMove(window, { clientX: 100, clientY: 80 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 10, y: 10, width: 40, height: 30 })
  expect(await screen.findByText('已學習 ID3')).toBeTruthy()
})

test('drag is clamped to the frame and tiny drags are ignored', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  fireEvent.pointerDown(stage, { clientX: 600, clientY: 400 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 900, clientY: 700 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 300, y: 200, width: 20, height: 40 })
  onLearn.mockClear()
  fireEvent.pointerDown(stage, { clientX: 100, clientY: 100 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 102, clientY: 102 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('no dragging when learning is unavailable', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn, canLearn: false })
  fireEvent.pointerDown(container.querySelector('.stage')!, { clientX: 0, clientY: 0 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 200, clientY: 200 })
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
    [new NotConnectedError(), '請先連線 MXCam'],
    [new LearnError('busy'), '學習中，請稍候'],
    [new LearnError('cancelled'), '學習已取消（功能已切換）'],
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

test('no crosshair over the placeholder while there is no frame (not connected)', () => {
  stubRect(640, 480)
  const { container } = setup({ connected: false })
  expect(container.querySelector('.ls-crosshair')).toBeNull()
  expect(screen.getByText('連線後會在這裡顯示即時畫面')).toBeInTheDocument()
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
  fireEvent.pointerDown(container.querySelector('.stage')!, { clientX: 20, clientY: 20 })
  fireEvent.keyDown(window, { key: 'Escape' })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 200, clientY: 200 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('review point 4: boxes are the results of the frame on screen, not the newest results', async () => {
  stubRect(320, 240)
  const ctx = { clearRect: () => {}, drawImage: () => {} }
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(ctx as unknown as CanvasRenderingContext2D)
  vi.stubGlobal('createImageBitmap', async () => ({ width: 320, height: 240, close: () => {} }))
  const store = new ResultsStore()
  const at = (id: number, x: number): Result[] => [{ id, kind: 'color', x, y: 100, width: 20, height: 20 }]
  store.push(at(1, 50), 10)
  store.push(at(2, 60), 11)
  store.push(at(3, 70), 12)
  const { ref, container } = setup({ results: store })
  const left = () => [...container.querySelectorAll<HTMLElement>('.ls-box')].map((b) => b.style.left)
  expect(left()).toEqual(['60px']) // nothing on screen yet: newest (x 70 - w/2)
  await act(async () => {
    ref.current!.draw({ kind: 'jpeg', jpeg: new Uint8Array([0xff, 0xd8]), frameId: 11 })
  })
  await vi.waitFor(() => expect(left()).toEqual(['50px'])) // frame 11's results (x 60)
  await act(async () => {
    store.push(at(4, 80), 13)
  })
  expect(left()).toEqual(['50px']) // newer results do not jump ahead of the picture
  vi.unstubAllGlobals()
})

// ---- mirror view (ADR-0010) ----
test('mirror view is on by default and persisted; it mirrors the canvas and the boxes but not the label text', () => {
  localStorage.removeItem('mxcam.mirror')
  stubRect(640, 480)
  const { container, unmount } = setup({ results: [{ id: 1, kind: 'color', x: 50, y: 100, width: 20, height: 40 }], learned: [{ id: 1, name: '紅' }] })
  const toggle = screen.getByRole('checkbox', { name: '鏡像顯示' })
  expect(toggle).toBeChecked()
  expect(container.querySelector('canvas')!.classList.contains('mirrored')).toBe(true)
  expect(container.querySelector<HTMLElement>('.ls-box')!.style.left).toBe('520px')
  expect(screen.getByText('ID1 紅')).toBeTruthy() // text itself is not mirrored
  fireEvent.click(toggle)
  expect(localStorage.getItem('mxcam.mirror')).toBe('0')
  expect(container.querySelector('canvas')!.classList.contains('mirrored')).toBe(false)
  expect(container.querySelector<HTMLElement>('.ls-box')!.style.left).toBe('80px')
  unmount()
  setup()
  expect(screen.getByRole('checkbox', { name: '鏡像顯示' })).not.toBeChecked()
  localStorage.removeItem('mxcam.mirror')
})

test('mirror view: dragging learns the TRUE-image region (x = 320 - x - w)', async () => {
  localStorage.removeItem('mxcam.mirror')
  stubRect(640, 480) // scale 2
  const onLearn = vi.fn(async () => 3)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  // on screen: frame-space 10..50 x 10..40 of the mirrored picture
  fireEvent.pointerDown(stage, { clientX: 20, clientY: 20 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 270, y: 10, width: 40, height: 30 })
})

test('the mirror toggle is there even when learning is unavailable', () => {
  setup({ canLearn: false })
  expect(screen.getByRole('checkbox', { name: '鏡像顯示' })).toBeInTheDocument()
})

test('gesture function: boxes get labels and dragging does not learn', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ fn: 8, canLearn: false, onLearn, results: [{ id: 6, kind: 'gesture', confidence: 92, x: 100, y: 100, width: 40, height: 40 }] })
  expect(screen.getByText('讚 92%')).toBeInTheDocument()
  const stage = container.querySelector('.stage')!
  fireEvent.pointerDown(stage, { clientX: 20, clientY: 20 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 100, clientY: 80 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('the pause toggle reports the new state', () => {
  const onPausePreview = vi.fn()
  setup({ onPausePreview })
  const box = screen.getByRole('checkbox', { name: '暫停預覽' })
  expect(box).not.toBeChecked()
  fireEvent.click(box)
  expect(onPausePreview).toHaveBeenLastCalledWith(true)
})

test('paused: placeholder note, boxes still drawn (mirrored), results rate instead of fps', () => {
  vi.useFakeTimers()
  try {
    localStorage.removeItem('mxcam.mirror') // mirror on
    stubRect(640, 480)
    const store = storeOf([{ id: 6, kind: 'gesture', confidence: 92, x: 100, y: 100, width: 40, height: 40 }])
    const { container } = setup({ fn: 8, canLearn: false, previewPaused: true, results: store, fps: 9.9 })
    expect(screen.getByText('預覽已暫停（辨識照常進行）')).toBeInTheDocument()
    expect(screen.getByText('讚 92%')).toBeInTheDocument()
    const boxEl = container.querySelector('.ls-box') as HTMLElement
    expect(boxEl.style.left).toBe('400px') // mirrored: true box 80..120 -> screen 200..240 frame px, x2
    expect(screen.getByRole('checkbox', { name: '暫停預覽' })).toBeChecked()
    expect(container.querySelector('.stage-stats')!.textContent).toContain('結果')
    act(() => {
      for (let i = 0; i < 4; i++) store.push([])
      vi.advanceTimersByTime(1000)
    })
    expect(container.querySelector('.stage-stats')!.textContent).toMatch(/結果 4\.0\/s/)
    expect(container.querySelector('.stage-stats')!.textContent).not.toContain('fps')
  } finally {
    vi.useRealTimers()
  }
})

test('not paused: no placeholder and the stats show fps', () => {
  const { container } = setup({ fps: 9.9 })
  expect(screen.queryByText('預覽已暫停（辨識照常進行）')).toBeNull()
  expect(container.querySelector('.stage-stats')!.textContent).toContain('9.9 fps')
})

test('pause race: a JPEG still decoding when the preview is paused does not set the displayed frame', async () => {
  stubRect(320, 240)
  const ctx = { clearRect: () => {}, drawImage: vi.fn() }
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(ctx as unknown as CanvasRenderingContext2D)
  let release: (b: { width: number; height: number; close: () => void }) => void = () => {}
  vi.stubGlobal('createImageBitmap', () => new Promise((res) => (release = res)))
  const store = new ResultsStore()
  const ref = createRef<LiveStageHandle>()
  const tree = (paused: boolean) => (
    <I18nProvider>
      <LiveStage ref={ref} connected fps={0} kbps={0} fn={1} canLearn={false} onLearn={async () => 1} results={store} learned={[]} showNames previewPaused={paused} />
    </I18nProvider>
  )
  const { rerender } = render(tree(false))
  act(() => {
    ref.current!.draw({ kind: 'jpeg', jpeg: new Uint8Array([0xff, 0xd8]), frameId: 5 })
  })
  rerender(tree(true))
  const spy = vi.spyOn(store, 'setDisplayed')
  const close = vi.fn()
  await act(async () => {
    release({ width: 320, height: 240, close })
    await Promise.resolve()
  })
  await vi.waitFor(() => expect(close).toHaveBeenCalled())
  expect(spy).not.toHaveBeenCalled()
  expect(ctx.drawImage).not.toHaveBeenCalled()
  vi.unstubAllGlobals()
})

test('the pause toggle is disabled while not connected', () => {
  setup({ connected: false })
  expect(screen.getByRole('checkbox', { name: '暫停預覽' })).toBeDisabled()
})

test('a gesture id 0 box (hand, not recognised) is drawn thin like an unlearned colour', () => {
  stubRect(640, 480)
  const { container } = setup({
    fn: 8,
    results: [
      { id: 0, kind: 'gesture', confidence: 81, x: 100, y: 100, width: 40, height: 40 },
      { id: 6, kind: 'gesture', confidence: 92, x: 200, y: 100, width: 40, height: 40 },
    ],
  })
  const boxes = [...container.querySelectorAll('.ls-box')]
  expect(boxes[0]).toHaveClass('unlearned')
  expect(boxes[1]).not.toHaveClass('unlearned')
})

// ---- line tracking (4) and object tracking (5) ----
const lineResult: Result = {
  id: 1, kind: 'line', x: 170, y: 150, width: 21, height: 181, confidence: 100,
  line: { angleDeg: -8, offsetX: 20, start: [180, 240], end: [160, 60], branch: false, cross: true },
}

test('line: draws a segment with its end points, the dashed ROI edge, the label and a crossing badge', () => {
  stubRect(320, 240)
  const { container } = setup({ fn: 4, canLearn: false, results: [lineResult], lineRoi: 60 })
  const seg = container.querySelector('line.ls-seg')!
  expect([seg.getAttribute('x1'), seg.getAttribute('y1'), seg.getAttribute('x2'), seg.getAttribute('y2')]).toEqual(['180', '240', '160', '60'])
  expect(Number(container.querySelector('circle.ls-end')!.getAttribute('cx'))).toBe(180)
  expect(Number(container.querySelector('line.ls-roi')!.getAttribute('y1'))).toBe(96) // 240 - 60% of 240
  expect(screen.getByText('角度 −8°・偏移 +20')).toBeInTheDocument()
  expect(screen.getByText('路口')).toBeInTheDocument()
  expect(screen.queryByText('岔路')).toBeNull()
  expect(container.querySelector('.ls-box')).toBeNull()
})

test('line: the dashed ROI edge is drawn even when no line is found', () => {
  stubRect(320, 240)
  const { container } = setup({ fn: 4, canLearn: false, results: [], lineRoi: 60 })
  expect(Number(container.querySelector('line.ls-roi')!.getAttribute('y1'))).toBe(96)
  expect(container.querySelector('line.ls-seg')).toBeNull()
})

test('line: mirrored view uses 320 - x for the end points', () => {
  localStorage.setItem('mxcam.mirror', '1')
  stubRect(320, 240)
  const { container } = setup({ fn: 4, canLearn: false, results: [lineResult] })
  const seg = container.querySelector('line.ls-seg')!
  expect([seg.getAttribute('x1'), seg.getAttribute('x2')]).toEqual(['140', '160'])
})

test('line: a branch gets its own badge; learning is available when canLearn', async () => {
  stubRect(320, 240)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ fn: 4, canLearn: true, onLearn, results: [{ ...lineResult, line: { ...lineResult.line!, cross: false, branch: true } }] })
  expect(screen.getByText('岔路')).toBeInTheDocument()
  fireEvent.pointerDown(container.querySelector('.stage')!, { clientX: 20, clientY: 20 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 20, y: 20, width: 80, height: 60 })
  expect(await screen.findByText('已學習線的顏色')).toBeInTheDocument()
})

test('track: a box labelled 物體 92%, the learned name when there is one, and the lost note', async () => {
  stubRect(320, 240)
  const r: Result = { id: 1, kind: 'track', x: 100, y: 100, width: 40, height: 40, confidence: 92 }
  const { container, unmount } = setup({ fn: 5, canLearn: true, results: [r] })
  expect(container.querySelectorAll('.ls-box')).toHaveLength(1)
  expect(screen.getByText('物體 92%')).toBeInTheDocument()
  expect(screen.getByText('物體追蹤')).toBeInTheDocument() // hat block
  unmount()
  setup({ fn: 5, results: [r], learned: [{ id: 1, name: '小車' }] })
  expect(screen.getByText('小車 92%')).toBeInTheDocument()
  expect(screen.queryByText('目標遺失')).toBeNull()
})

test('track: learned but no result shows 目標遺失; never learned does not', () => {
  const { unmount } = setup({ fn: 5, results: [], learned: [{ id: 1, name: '' }] })
  expect(screen.getByText('目標遺失')).toBeInTheDocument()
  unmount()
  setup({ fn: 5, results: [], learned: [] })
  expect(screen.queryByText('目標遺失')).toBeNull()
})

test('no-target error has its own message', async () => {
  const onLearn = vi.fn(async () => {
    throw new LearnError('no-target')
  })
  const { container } = setup({ fn: 5, onLearn })
  stubRect(320, 240)
  fireEvent.pointerDown(container.querySelector('.stage')!, { clientX: 20, clientY: 20 })
  await act(async () => {
    fireEvent.pointerUp(window, { clientX: 100, clientY: 80 })
  })
  expect(await screen.findByText(/找不到可追蹤的物體/)).toBeInTheDocument()
})

// ---- touch and pen (ADR-0023): the box is drawn with Pointer Events ----
/** jsdom has no pointer capture; these stand in for it so the calls can be seen. */
function stubCapture() {
  const proto = HTMLElement.prototype as unknown as Record<string, unknown>
  const set = vi.fn()
  const release = vi.fn()
  proto.setPointerCapture = set
  proto.releasePointerCapture = release
  return { set, release }
}
afterEach(() => {
  const proto = HTMLElement.prototype as unknown as Record<string, unknown>
  delete proto.setPointerCapture
  delete proto.releasePointerCapture
})

test.each(['mouse', 'touch', 'pen'] as const)('%s: pointer down / move / up learns the same region, with the pointer captured', async (pointerType) => {
  stubRect(640, 480) // scale 2
  const cap = stubCapture()
  const onLearn = vi.fn(async () => 3)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  const p = { pointerId: 7, pointerType, button: 0 }
  fireEvent.pointerDown(stage, { ...p, clientX: 20, clientY: 20 })
  expect(cap.set).toHaveBeenCalledWith(7)
  // Captured, the moves arrive at the stage (and bubble); the box follows them.
  fireEvent.pointerMove(stage, { ...p, clientX: 60, clientY: 50 })
  const box = container.querySelector<HTMLElement>('.ls-drag')!
  expect([box.style.left, box.style.top, box.style.width, box.style.height]).toEqual(['20px', '20px', '40px', '30px'])
  await act(async () => {
    fireEvent.pointerUp(stage, { ...p, clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledTimes(1)
  expect(onLearn).toHaveBeenCalledWith({ x: 10, y: 10, width: 40, height: 30 }) // the region of the first drag test above
  expect(cap.release).toHaveBeenCalledWith(7)
  expect(container.querySelector('.ls-drag')).toBeNull()
  expect(await screen.findByText('已學習 ID3')).toBeTruthy()
})

test('touch with mirror view on: the box learns the TRUE-image region, as the mouse does', async () => {
  localStorage.removeItem('mxcam.mirror') // mirror on (the default)
  stubRect(640, 480)
  stubCapture()
  const onLearn = vi.fn(async () => 3)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  const p = { pointerId: 3, pointerType: 'touch', button: 0 }
  fireEvent.pointerDown(stage, { ...p, clientX: 20, clientY: 20 })
  fireEvent.pointerMove(stage, { ...p, clientX: 100, clientY: 80 })
  // The box is under the finger, on the mirrored picture.
  expect(container.querySelector<HTMLElement>('.ls-drag')!.style.left).toBe('20px')
  await act(async () => {
    fireEvent.pointerUp(stage, { ...p, clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 270, y: 10, width: 40, height: 30 })
})

test('a tap without movement does not learn', async () => {
  stubRect(640, 480)
  stubCapture()
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  const p = { pointerId: 1, pointerType: 'touch', button: 0, clientX: 200, clientY: 150 }
  fireEvent.pointerDown(stage, p)
  await act(async () => {
    fireEvent.pointerUp(stage, p)
  })
  expect(onLearn).not.toHaveBeenCalled()
  expect(container.querySelector('.ls-drag')).toBeNull()
  expect(screen.queryByRole('status')).toBeNull() // no message either: nothing new happens
})

test('the browser taking the touch away (pointercancel) drops the box without learning', async () => {
  stubRect(640, 480)
  const cap = stubCapture()
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  const p = { pointerId: 1, pointerType: 'touch', button: 0 }
  fireEvent.pointerDown(stage, { ...p, clientX: 20, clientY: 20 })
  fireEvent.pointerMove(stage, { ...p, clientX: 200, clientY: 200 })
  expect(container.querySelector('.ls-drag')).not.toBeNull()
  fireEvent.pointerCancel(stage, p)
  expect(container.querySelector('.ls-drag')).toBeNull()
  expect(cap.release).toHaveBeenCalledWith(1)
  await act(async () => {
    fireEvent.pointerUp(stage, { ...p, clientX: 200, clientY: 200 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('a second finger while a box is being drawn neither moves nor finishes it', async () => {
  stubRect(640, 480)
  const cap = stubCapture()
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const stage = container.querySelector('.stage')!
  const first = { pointerId: 1, pointerType: 'touch', button: 0 }
  const second = { pointerId: 2, pointerType: 'touch', button: 0 }
  fireEvent.pointerDown(stage, { ...first, clientX: 20, clientY: 20 })
  fireEvent.pointerDown(stage, { ...second, clientX: 400, clientY: 400 })
  expect(cap.set).toHaveBeenCalledTimes(1)
  fireEvent.pointerMove(stage, { ...second, clientX: 500, clientY: 450 })
  await act(async () => {
    fireEvent.pointerUp(stage, { ...second, clientX: 500, clientY: 450 })
  })
  expect(onLearn).not.toHaveBeenCalled()
  await act(async () => {
    fireEvent.pointerUp(stage, { ...first, clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 10, y: 10, width: 40, height: 30 })
})

test('no pointer capture in the browser: the box is still drawn from the window events', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  const p = { pointerId: 1, pointerType: 'touch', button: 0 }
  fireEvent.pointerDown(container.querySelector('.stage')!, { ...p, clientX: 20, clientY: 20 })
  await act(async () => {
    fireEvent.pointerUp(window, { ...p, clientX: 100, clientY: 80 })
  })
  expect(onLearn).toHaveBeenCalledWith({ x: 10, y: 10, width: 40, height: 30 })
})

test('a mouse press keeps its old default handling: only the left button on a learnable stage is prevented', () => {
  const { container, unmount } = setup()
  const stage = container.querySelector('.stage')!
  expect(fireEvent.mouseDown(stage, { button: 0 })).toBe(false) // preventDefault(): no focus change, no text selection
  expect(fireEvent.mouseDown(stage, { button: 2 })).toBe(true)
  unmount()
  const off = setup({ canLearn: false })
  expect(fireEvent.mouseDown(off.container.querySelector('.stage')!, { button: 0 })).toBe(true)
})

test('the right mouse button does not start a box', async () => {
  stubRect(640, 480)
  const onLearn = vi.fn(async () => 1)
  const { container } = setup({ onLearn })
  fireEvent.pointerDown(container.querySelector('.stage')!, { pointerId: 1, pointerType: 'mouse', button: 2, clientX: 20, clientY: 20 })
  expect(container.querySelector('.ls-drag')).toBeNull()
  await act(async () => {
    fireEvent.pointerUp(window, { pointerId: 1, pointerType: 'mouse', button: 2, clientX: 100, clientY: 80 })
  })
  expect(onLearn).not.toHaveBeenCalled()
})

test('the stage only takes touches over (class `learnable`; touch-action: none in theme.css) while a box can be drawn', () => {
  const { container, unmount } = setup({ canLearn: true })
  expect(container.querySelector('.stage')).toHaveClass('learnable')
  unmount()
  expect(setup({ canLearn: false, fn: 8 }).container.querySelector('.stage')).not.toHaveClass('learnable')
})
