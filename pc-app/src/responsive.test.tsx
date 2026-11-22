// Phones and tablets (docs/adr/0023). jsdom does not lay out CSS, so these tests check behaviour and the
// structure the narrow layout in theme.css depends on, not pixels. The stylesheet's own facts (breakpoints, sticky
// parts, touch rules) are checked in ../theme.css.test.ts.
import { act, fireEvent, render, screen, within } from '@testing-library/react'
import type { ComponentProps } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import App from './App'
import { I18nProvider } from './i18n'
import { translate } from './i18n/core'
import { FakeFirmware } from './testing/fakeFirmware'
import Sidebar from './ui/layout/Sidebar'

function renderApp(props: ComponentProps<typeof App> = {}) {
  return render(
    <I18nProvider>
      <App {...props} />
    </I18nProvider>,
  )
}

const idleSerial = {
  addEventListener: () => {},
  removeEventListener: () => {},
  getPorts: async () => [],
  requestPort: async () => {
    throw new DOMException('cancel', 'NotFoundError')
  },
} as unknown as Serial

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
})
afterEach(() => vi.useRealTimers())

// ---- a greyed-out function says why when it is tapped (touch has no hover) ----

test('sidebar: tapping an unavailable function reports the hover hint; an available one just selects', () => {
  const onSelect = vi.fn()
  const onUnavailable = vi.fn()
  const { rerender } = render(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[1]} onSelect={onSelect} onUnavailable={onUnavailable} />
    </I18nProvider>,
  )
  const face = screen.getByRole('button', { name: '人臉' })
  fireEvent.click(face)
  expect(onUnavailable).toHaveBeenLastCalledWith('needFirmware')
  expect(translate('zh-TW', 'needFirmware')).toBe(face.getAttribute('title')) // the same text as the hover hint
  expect(onSelect).not.toHaveBeenCalled()
  fireEvent.click(screen.getByRole('button', { name: '顏色' }))
  expect(onSelect).toHaveBeenCalledWith({ kind: 'function', id: 1 })
  expect(onUnavailable).toHaveBeenCalledTimes(1)
  rerender(
    <I18nProvider>
      <Sidebar view={{ kind: 'function', id: 1 }} supported={[]} needConnect onSelect={onSelect} onUnavailable={onUnavailable} />
    </I18nProvider>,
  )
  fireEvent.click(screen.getByRole('button', { name: '人臉' }))
  expect(onUnavailable).toHaveBeenLastCalledWith('needConnect')
})

test('real board without that function: a tap shows 需要更新韌體 in the notice line under the top bar', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2]
  const { container } = renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  expect(container.querySelector('[data-slot="fn-hint"]')).toBeNull()
  fireEvent.click(screen.getByRole('button', { name: '手勢' }))
  const hint = container.querySelector('[data-slot="fn-hint"]')!
  expect(hint).toHaveTextContent('需要更新韌體')
  // The place the app already uses for short notices: a status line above the columns.
  expect(hint).toHaveAttribute('role', 'status')
  expect(hint).toHaveClass('app-info')
  expect(hint.parentElement).toBe(container.querySelector('.app'))
  expect(hint.nextElementSibling).toBe(container.querySelector('.app-columns'))
  // The view did not change.
  expect(screen.getByRole('button', { name: '顏色' })).toHaveAttribute('aria-current', 'page')
})

test('the tap notice goes away by itself, and at once when another item is chosen', () => {
  vi.useFakeTimers()
  const { container } = renderApp({ serial: idleSerial })
  const hint = () => container.querySelector('[data-slot="fn-hint"]')
  fireEvent.click(screen.getByRole('button', { name: '人臉' }))
  expect(hint()).toHaveTextContent('請先連線 MXCam') // not connected: the hint is "connect first"
  act(() => {
    vi.advanceTimersByTime(3900)
  })
  expect(hint()).not.toBeNull()
  act(() => {
    vi.advanceTimersByTime(200)
  })
  expect(hint()).toBeNull()
  fireEvent.click(screen.getByRole('button', { name: '人臉' }))
  expect(hint()).not.toBeNull()
  fireEvent.click(screen.getByRole('button', { name: '設定' }))
  expect(hint()).toBeNull()
})

// ---- no Web Serial and no Web Bluetooth (every iPhone / iPad browser) ----

test('neither API: the text says this device cannot connect and that demo mode works, in both languages', () => {
  renderApp()
  const zh = translate('zh-TW', 'unsupported')
  expect(zh).toMatch(/連不上 MXCam/)
  expect(zh).toMatch(/展示模式/)
  expect(screen.getByText(zh)).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: /連線/ })).toBeNull()
  fireEvent.click(screen.getByRole('button', { name: 'English' }))
  const en = translate('en', 'unsupported')
  expect(en).toMatch(/cannot connect to MXCam/)
  expect(en).toMatch(/[Dd]emo mode/)
  expect(screen.getByText(en)).toBeInTheDocument()
})

test('neither API: demo mode can be turned on from the notice itself', async () => {
  const { container } = renderApp()
  const notice = container.querySelector<HTMLElement>('.app-notice')!
  fireEvent.click(within(notice).getByRole('button', { name: '開啟展示模式' }))
  expect(await screen.findByText('DEMO')).toBeInTheDocument()
  expect(container.querySelector('.app-notice')).toBeNull()
  expect(container.querySelector('.stage')).not.toBeNull()
  expect(screen.getByRole('switch', { name: '展示模式' })).toHaveAttribute('aria-checked', 'true')
  expect(localStorage.getItem('mxcam.demo')).toBe('1')
})

test('neither API: the demo switch in the top bar is there too, and turning demo off brings the notice back', async () => {
  renderApp()
  const toggle = screen.getByRole('switch', { name: '展示模式' })
  expect(toggle).toHaveAttribute('aria-checked', 'false')
  fireEvent.click(toggle)
  expect(await screen.findByText('DEMO')).toBeInTheDocument()
  expect(screen.queryByText(translate('zh-TW', 'unsupported'))).toBeNull()
  fireEvent.click(screen.getByRole('switch', { name: '展示模式' }))
  expect(await screen.findByText(translate('zh-TW', 'unsupported'))).toBeInTheDocument()
})

// ---- the structure the narrow layout depends on ----

/** Direct children of `el` as "tag.firstClass", in DOM order. */
const kids = (el: Element | null) => [...(el?.children ?? [])].map((c) => `${c.tagName.toLowerCase()}.${c.classList[0] ?? ''}`)

test('narrow layout contract: the order and class names theme.css lays out as one column', async () => {
  localStorage.setItem('mxcam.demo', '1')
  const { container } = renderApp({ serial: idleSerial })
  await screen.findByText('DEMO')
  const app = container.querySelector('.app')!
  // Top bar first, the columns last (notices go between them).
  expect(kids(app)).toEqual(['header.topbar', 'div.app-columns'])
  // One column, top to bottom: function strip, picture, panel.
  expect(kids(app.querySelector('.app-columns'))).toEqual(['nav.sidebar', 'main.app-center', 'aside.rightpanel'])
  // .app-center, .stage-wrap and .rightpanel are `display: contents` when narrow, so these become the rows:
  expect(kids(app.querySelector('.app-center'))).toEqual(['div.stage-wrap'])
  expect(kids(app.querySelector('.stage-wrap'))).toEqual(['div.ls-hat', 'div.ls-pin', 'div.stage-actions'])
  // The pinned band holds the picture and nothing else.
  expect(kids(app.querySelector('.ls-pin'))).toEqual(['div.stage'])
  // Tabs, the tab content, then the save bar (sticky at the bottom of the screen).
  expect(kids(app.querySelector('.rightpanel'))).toEqual(['div.rp-tabs', 'div.rp-tabpanel', 'div.rp-savebar'])
  expect(app.querySelector('.rp-savebar .rp-saverow .btn')).toHaveTextContent('存到板子')
})

test('narrow layout contract: the function strip keeps settings and firmware at its end, after a rule', () => {
  localStorage.setItem('mxcam.demo', '1')
  const { container } = renderApp({ serial: idleSerial })
  const strip = container.querySelector('nav.sidebar')!
  const order = [...strip.children].filter((c) => !c.classList.contains('visually-hidden')).map((c) => (c.classList.contains('nav-rule') ? '|' : c.textContent))
  expect(order.slice(-3)).toEqual(['|', '設定', '韌體'])
  expect(order.indexOf('|')).toBe(order.length - 3)
  // Every item: a category disc and a label (the strip shows both).
  for (const item of strip.querySelectorAll('.nav-item')) {
    expect(item.querySelector('.nav-disc')).not.toBeNull()
    expect(item.querySelector('.nav-label')).not.toBeNull()
  }
  expect(strip.querySelectorAll('[aria-current="page"]')).toHaveLength(1)
})

test('narrow layout contract: the top bar groups (name / status / switches + connect buttons)', () => {
  const fw = new FakeFirmware()
  const { container } = renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  const bar = container.querySelector('header.topbar')!
  expect(kids(bar)).toEqual(['div.tb-start', 'div.tb-centre', 'div.tb-end'])
  expect(bar.querySelector('.tb-start .brand')).toHaveTextContent('MXCam Studio')
  expect(bar.querySelector('.tb-centre .conn-status .lamp')).not.toBeNull()
  // .tb-end is `display: contents` when narrow; its three children wrap as separate items.
  expect(kids(bar.querySelector('.tb-end'))).toEqual(['button.tb-switch', 'div.tb-seg', 'div.tb-connect'])
  const connect = within(bar.querySelector<HTMLElement>('.tb-connect')!)
  // Every button keeps its text label (and so its accessible name) at any width.
  for (const name of ['USB 連線', '藍牙連線', '中斷']) expect(connect.getByRole('button', { name })).toBeInTheDocument()
})

test('settings and firmware pages keep the classes the single-column rules use', () => {
  localStorage.setItem('mxcam.demo', '1')
  const { container } = renderApp({ serial: idleSerial })
  fireEvent.click(screen.getByRole('button', { name: '設定' }))
  const main = container.querySelector('main.app-center')!
  expect(kids(main)).toEqual(['section.pg'])
  expect(main.querySelectorAll('.pg-section').length).toBeGreaterThan(0)
  expect(main.querySelector('.pg-row .pg-seg')).not.toBeNull()
  expect(main.querySelector('.pg-row .pg-hex input')).not.toBeNull()
  // The panel and the save bar stay on the page under it.
  expect(container.querySelector('.rightpanel .rp-savebar')).not.toBeNull()
  fireEvent.click(screen.getByRole('button', { name: '韌體' }))
  expect(kids(main)).toEqual(['section.pg'])
  expect(main.querySelectorAll('.pg-tiles > .pg-tile')).toHaveLength(2)
})
