import { fireEvent, render, screen, within } from '@testing-library/react'
import { beforeEach, expect, test, vi } from 'vitest'
import { I18nProvider } from '../../i18n'
import RightPanel from './RightPanel'
import Sidebar from './Sidebar'
import TopBar from './TopBar'
import { ResultsStore } from '../../device/resultsStore'
import { defaultParams, defaultSystem, emptyLearned, type DeviceSnapshot } from '../../device/types'

const idleSnapshot: DeviceSnapshot = {
  mode: 'demo',
  status: 'connected',
  supported: [1, 2, 3],
  activeFunction: 1,
  learned: emptyLearned(),
  params: defaultParams(),
  system: defaultSystem(),
  dirty: false,
  lost: false,
  previewPaused: false,
  configKnown: true,
  transports: ['usb'],
  transport: 'usb',
}

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
  document.documentElement.lang = ''
})

test('sidebar greys out unsupported functions and does not select them', () => {
  const onSelect = vi.fn()
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1]} onSelect={onSelect} />
    </I18nProvider>,
  )
  for (const name of ['AprilTag', '人臉']) {
    const b = screen.getByRole('button', { name })
    expect(b).toHaveAttribute('aria-disabled', 'true')
    expect(b).toHaveAttribute('title', '需要更新韌體')
    fireEvent.click(b)
  }
  expect(onSelect).not.toHaveBeenCalled()
  fireEvent.click(screen.getByRole('button', { name: '設定' }))
  expect(onSelect).toHaveBeenCalledWith({ kind: 'settings' })
})

test('sidebar: needConnect shows 請先連線 MXCam, and disabled items carry an aria-describedby description', () => {
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[]} needConnect onSelect={() => {}} />
    </I18nProvider>,
  )
  const b = screen.getByRole('button', { name: '人臉' })
  expect(b).toHaveAttribute('title', '請先連線 MXCam')
  expect(b).toHaveAccessibleDescription('請先連線 MXCam')
  expect(screen.queryByText('需要更新韌體')).toBeNull()
})

test('sidebar: connected but unsupported describes needFirmware', () => {
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1]} onSelect={() => {}} />
    </I18nProvider>,
  )
  expect(screen.getByRole('button', { name: '人臉' })).toHaveAccessibleDescription('需要更新韌體')
  expect(screen.getByRole('button', { name: '顏色' })).not.toHaveAttribute('aria-describedby')
})

test('language buttons carry their own lang attribute', () => {
  render(
    <I18nProvider>
      <TopBar status="connected" lost={false} info={null} demo={false} onToggleDemo={() => {}} onConnect={() => {}} onDisconnect={() => {}} />
    </I18nProvider>,
  )
  expect(screen.getByRole('button', { name: '繁中' })).toHaveAttribute('lang', 'zh-Hant')
  expect(screen.getByRole('button', { name: 'English' })).toHaveAttribute('lang', 'en')
})

test('nav landmark is named as general navigation in both languages', () => {
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'settings' }} supported={[1, 2, 3]} onSelect={() => {}} />
    </I18nProvider>,
  )
  expect(screen.getByRole('navigation', { name: '導覽' })).toBeInTheDocument()
})

test('tabs use roving tabindex and arrow/Home/End keys move and select', () => {
  render(
    <I18nProvider>
      <RightPanel device={null} snapshot={idleSnapshot} fn={1} results={new ResultsStore()} />
    </I18nProvider>,
  )
  const tab = (n: string) => screen.getByRole('tab', { name: n })
  expect(tab('結果')).toHaveAttribute('tabindex', '0')
  expect(tab('學習')).toHaveAttribute('tabindex', '-1')
  fireEvent.keyDown(tab('結果'), { key: 'ArrowRight' })
  expect(tab('學習')).toHaveAttribute('aria-selected', 'true')
  expect(tab('學習')).toHaveFocus()
  expect(tab('學習')).toHaveAttribute('tabindex', '0')
  expect(tab('結果')).toHaveAttribute('tabindex', '-1')
  fireEvent.keyDown(tab('學習'), { key: 'ArrowLeft' })
  fireEvent.keyDown(tab('結果'), { key: 'ArrowLeft' })
  expect(tab('程式')).toHaveAttribute('aria-selected', 'true')
  fireEvent.keyDown(tab('程式'), { key: 'ArrowRight' })
  expect(tab('結果')).toHaveAttribute('aria-selected', 'true')
  fireEvent.keyDown(tab('結果'), { key: 'End' })
  expect(tab('程式')).toHaveFocus()
  fireEvent.keyDown(tab('程式'), { key: 'Home' })
  expect(tab('結果')).toHaveFocus()
  expect(tab('結果')).toHaveAttribute('aria-selected', 'true')
})

test('html lang follows the UI language', () => {
  render(
    <I18nProvider>
      <div />
    </I18nProvider>,
  )
  expect(document.documentElement.lang).toBe('zh-TW')
})

test('sidebar has a gesture item with the Hand icon and its own category colour', () => {
  const { container } = render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3, 8]} onSelect={() => {}} />
    </I18nProvider>,
  )
  const b = screen.getByRole('button', { name: '手勢' })
  expect(b).not.toHaveAttribute('aria-disabled')
  expect(b.querySelector('svg')!.getAttribute('class')).toContain('lucide-hand')
  expect((b.querySelector('.nav-disc') as HTMLElement).style.background).toBe('var(--cat-gesture)')
  expect(container.querySelectorAll('.nav-item')).toHaveLength(11) // 9 functions + settings + firmware
})

test('gesture item is disabled with the firmware hint when the board does not list it', () => {
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3]} onSelect={() => {}} />
    </I18nProvider>,
  )
  expect(screen.getByRole('button', { name: '手勢' })).toHaveAttribute('aria-disabled', 'true')
  expect(screen.getByRole('button', { name: '手勢' })).toHaveAccessibleDescription('需要更新韌體')
})

test('sidebar lists line and object tracking; old firmware (1,2,3) disables them with a firmware hint', () => {
  const onSelect = vi.fn()
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3]} onSelect={onSelect} />
    </I18nProvider>,
  )
  for (const name of ['線條追蹤', '物體追蹤']) {
    const b = screen.getByRole('button', { name })
    expect(b).toHaveAttribute('aria-disabled', 'true')
    expect(b).toHaveAccessibleDescription('需要更新韌體')
    fireEvent.click(b)
  }
  expect(onSelect).not.toHaveBeenCalled()
})

test('sidebar: with line and track supported they select', () => {
  const onSelect = vi.fn()
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3, 4, 5]} onSelect={onSelect} />
    </I18nProvider>,
  )
  fireEvent.click(screen.getByRole('button', { name: '線條追蹤' }))
  fireEvent.click(screen.getByRole('button', { name: '物體追蹤' }))
  expect(onSelect).toHaveBeenNthCalledWith(1, { kind: 'function', id: 4 })
  expect(onSelect).toHaveBeenNthCalledWith(2, { kind: 'function', id: 5 })
})

// ---- M7: QR code (6), shapes (11), motion (12) ----
test('function list has 9 items: QR after AprilTag, shapes and motion last, each with its icon and category colour', () => {
  const { container } = render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3, 4, 5, 6, 8, 11, 12]} onSelect={() => {}} />
    </I18nProvider>,
  )
  const nav = screen.getByRole('navigation')
  const fnItems = [...container.querySelectorAll('.nav-item')].slice(0, 9).map((b) => b.textContent)
  expect(fnItems).toEqual(['顏色', 'AprilTag', 'QR Code', '人臉', '線條追蹤', '物體追蹤', '手勢', '幾何形狀', '動態偵測'])
  const cases = [
    ['QR Code', 'lucide-scan-qr-code', 'var(--cat-qr)'],
    ['幾何形狀', 'lucide-shapes', 'var(--cat-shape)'],
    ['動態偵測', 'lucide-activity', 'var(--cat-motion)'],
  ] as const
  for (const [name, icon, color] of cases) {
    const b = within(nav).getByRole('button', { name })
    expect(b).not.toHaveAttribute('aria-disabled')
    expect(b.querySelector('svg')!.getAttribute('class')).toContain(icon)
    expect((b.querySelector('.nav-disc') as HTMLElement).style.background).toBe(color)
  }
})

test('M7 functions select, and older firmware (1,2,3,4,5,8) greys them out with the firmware hint', () => {
  const onSelect = vi.fn()
  const { unmount } = render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3, 4, 5, 6, 8, 11, 12]} onSelect={onSelect} />
    </I18nProvider>,
  )
  for (const name of ['QR Code', '幾何形狀', '動態偵測']) fireEvent.click(screen.getByRole('button', { name }))
  expect(onSelect.mock.calls.map((c) => c[0])).toEqual([
    { kind: 'function', id: 6 },
    { kind: 'function', id: 11 },
    { kind: 'function', id: 12 },
  ])
  unmount()
  onSelect.mockClear()
  render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1, 2, 3, 4, 5, 8]} onSelect={onSelect} />
    </I18nProvider>,
  )
  for (const name of ['QR Code', '幾何形狀', '動態偵測']) {
    const b = screen.getByRole('button', { name })
    expect(b).toHaveAttribute('aria-disabled', 'true')
    expect(b).toHaveAccessibleDescription('需要更新韌體')
    fireEvent.click(b)
  }
  expect(onSelect).not.toHaveBeenCalled()
})
