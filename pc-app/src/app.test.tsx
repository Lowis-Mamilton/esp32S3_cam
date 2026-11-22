import { fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { FakeFirmware } from './testing/fakeFirmware'
import { Decoder, encodeFrame } from './protocol/codec'
import { Cmd, ErrorCode, responseOf } from './protocol/messages'
import { beforeEach, expect, test } from 'vitest'
import type { ComponentProps } from 'react'
import App from './App'
import { I18nProvider } from './i18n'
import { translate } from './i18n/core'

function renderApp(props: ComponentProps<typeof App> = {}) {
  return render(
    <I18nProvider>
      <App {...props} />
    </I18nProvider>,
  )
}

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
})

const idleSerial = {
  addEventListener: () => {},
  removeEventListener: () => {},
  getPorts: async () => [],
  requestPort: async () => {
    throw new DOMException('cancel', 'NotFoundError')
  },
} as unknown as Serial

test('renders the MXCam Studio heading', () => {
  renderApp()
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})

test('renders a single heading per test (cleanup between tests)', () => {
  renderApp()
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})

test('without navigator.serial shows the unsupported message and no connect button', () => {
  renderApp()
  expect(screen.getByText('此瀏覽器不支援，請用 Chrome 或 Edge')).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: '連線' })).not.toBeInTheDocument()
})

test('when disconnected shows the connect button and 未連線 status', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('button', { name: '連線' })).toBeEnabled()
  expect(screen.getByRole('button', { name: '中斷' })).toBeDisabled()
  expect(screen.getByText('未連線')).toBeInTheDocument()
  expect(screen.queryByText('此瀏覽器不支援，請用 Chrome 或 Edge')).not.toBeInTheDocument()
})

test('shows 已斷線 after the read loop ends unexpectedly', async () => {
  let ctl!: ReadableStreamDefaultController<Uint8Array>
  const dec = new Decoder()
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    open: async () => {},
    close: async () => {},
    readable: new ReadableStream<Uint8Array>({
      start: (c) => {
        ctl = c
      },
    }),
    writable: new WritableStream<Uint8Array>({
      write: (chunk) => {
        for (const f of dec.push(chunk)) {
          // M1 firmware: M2 commands (GET_CONFIG ...) are UNKNOWN_CMD
          if (f.cmd === Cmd.GetConfig) ctl.enqueue(encodeFrame(Cmd.Error, f.seq, new Uint8Array([f.cmd, ErrorCode.UnknownCmd])))
          else ctl.enqueue(encodeFrame(responseOf(f.cmd), f.seq, f.cmd === Cmd.GetInfo ? new Uint8Array(23) : new Uint8Array(0)))
        }
      },
    }),
  }
  const serial = { ...idleSerial, getPorts: async () => [port] } as unknown as Serial
  renderApp({ serial })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  ctl.close()
  expect(await screen.findByText('已斷線')).toBeInTheDocument()
})

test('port busy (NetworkError on open) shows the Chinese busy message', async () => {
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    open: async () => {
      throw new DOMException('Failed to open serial port.', 'NetworkError')
    },
    close: async () => {},
  }
  const serial = { ...idleSerial, requestPort: async () => port } as unknown as Serial
  renderApp({ serial })
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  expect(await screen.findByRole('alert')).toHaveTextContent('無法開啟序列埠：可能被其他分頁或程式佔用')
})

test('defaults to zh-TW and switches to English, persisting the choice', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('button', { name: '顏色' })).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: 'English' }))
  expect(screen.getByRole('button', { name: 'Color' })).toBeInTheDocument()
  expect(screen.getByRole('button', { name: 'Connect' })).toBeInTheDocument()
  // Only the language switch itself keeps its native label (繁中).
  const leftover = screen
    .queryAllByText(/[一-鿿]/)
    .filter((el) => !el.closest('[role="group"]'))
  expect(leftover).toEqual([])
  expect(localStorage.getItem('mxcam.lang')).toBe('en')
})

test('restores the saved language', () => {
  localStorage.setItem('mxcam.lang', 'en')
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('button', { name: 'Color' })).toBeInTheDocument()
})

test('has the three columns: sidebar, live area and right panel tabs', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('navigation')).toBeInTheDocument()
  expect(screen.getByRole('main')).toBeInTheDocument()
  expect(screen.getByRole('complementary')).toBeInTheDocument()
  for (const name of ['結果', '學習', '參數', '程式']) expect(screen.getByRole('tab', { name })).toBeInTheDocument()
})

test('right panel tabs switch', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('tab', { name: '結果' })).toHaveAttribute('aria-selected', 'true')
  fireEvent.click(screen.getByRole('tab', { name: '參數' }))
  expect(screen.getByRole('tab', { name: '參數' })).toHaveAttribute('aria-selected', 'true')
})

test('sidebar greys out all functions while no device is connected', () => {
  renderApp({ serial: idleSerial })
  for (const name of ['顏色', 'AprilTag', '人臉']) {
    expect(screen.getByRole('button', { name })).toHaveAttribute('aria-disabled', 'true')
  }
})

test('demo toggle shows the badge, DEMO firmware, hides connect and persists; turning it off returns to 未連線', async () => {
  renderApp({ serial: idleSerial })
  const toggle = screen.getByRole('switch', { name: '展示模式' })
  expect(toggle).toHaveAttribute('aria-checked', 'false')
  fireEvent.click(toggle)
  expect(await screen.findByText('DEMO')).toBeInTheDocument()
  expect(screen.getByText('展示模式', { selector: '.demo-badge' })).toBeInTheDocument()
  expect(screen.getByText('已連線')).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: '連線' })).not.toBeInTheDocument()
  expect(screen.getByRole('button', { name: '人臉' })).not.toHaveAttribute('aria-disabled')
  expect(localStorage.getItem('mxcam.demo')).toBe('1')
  fireEvent.click(screen.getByRole('switch', { name: '展示模式' }))
  expect(await screen.findByText('未連線')).toBeInTheDocument()
  expect(document.querySelector('.demo-badge')).toBeNull()
  expect(screen.getByRole('button', { name: '連線' })).toBeEnabled()
})

test('demo badge and toggle are translated', async () => {
  localStorage.setItem('mxcam.lang', 'en')
  localStorage.setItem('mxcam.demo', '1')
  renderApp({ serial: idleSerial })
  expect(await screen.findByText('Demo mode', { selector: '.demo-badge' })).toBeInTheDocument()
  expect(screen.getByRole('switch', { name: 'Demo mode' })).toHaveAttribute('aria-checked', 'true')
})

test('sidebar switches views between function, settings and firmware', () => {
  localStorage.setItem('mxcam.demo', '1')
  renderApp({ serial: idleSerial })
  fireEvent.click(screen.getByRole('button', { name: '設定' }))
  expect(screen.getByRole('heading', { name: '系統設定' })).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '韌體' }))
  expect(screen.getByRole('heading', { name: '韌體更新' })).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '人臉' }))
  expect(screen.getByRole('button', { name: '人臉' })).toHaveAttribute('aria-current', 'page')
})

test('translate has the unsupported text in both languages', () => {
  expect(translate('en', 'unsupported')).not.toBe(translate('zh-TW', 'unsupported'))
})

function slowPortSerial(open: () => Promise<void>): Serial {
  const port = { getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }), open, close: async () => {} }
  return { ...idleSerial, requestPort: async () => port } as unknown as Serial
}

test('no error is shown when a connect rejection follows a user-initiated disconnect', async () => {
  let rejectOpen!: (e: unknown) => void
  const serial = slowPortSerial(() => new Promise<void>((_, rej) => (rejectOpen = rej)))
  renderApp({ serial })
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  expect(await screen.findByText('連線中…')).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '中斷' }))
  rejectOpen(new DOMException('Failed to open serial port.', 'NetworkError'))
  expect(await screen.findByText('未連線')).toBeInTheDocument()
  await new Promise((r) => setTimeout(r, 20))
  expect(screen.queryByRole('alert')).not.toBeInTheDocument()
})

test('the error is cleared when a new connection attempt starts', async () => {
  let n = 0
  let release: (() => void) | undefined
  const serial = slowPortSerial(() =>
    ++n === 1
      ? Promise.reject(new DOMException('busy', 'NetworkError'))
      : new Promise<void>((res) => (release = res)),
  )
  renderApp({ serial })
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  expect(await screen.findByRole('alert')).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  expect(await screen.findByText('連線中…')).toBeInTheDocument()
  expect(screen.queryByRole('alert')).not.toBeInTheDocument()
  release?.()
})

test('StrictMode fresh load with no granted ports shows 未連線, not 已斷線', async () => {
  const { StrictMode } = await import('react')
  render(
    <StrictMode>
      <I18nProvider>
        <App serial={idleSerial} />
      </I18nProvider>
    </StrictMode>,
  )
  await new Promise((r) => setTimeout(r, 50))
  expect(screen.getByText('未連線')).toBeInTheDocument()
  expect(screen.queryByText('已斷線')).not.toBeInTheDocument()
})

test('StrictMode fresh load where the granted port fails to open shows 未連線, not 已斷線', async () => {
  const { StrictMode } = await import('react')
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    open: async () => {
      throw new DOMException('Failed to open serial port.', 'NetworkError')
    },
    close: async () => {},
  }
  const serial = { ...idleSerial, getPorts: async () => [port] } as unknown as Serial
  render(
    <StrictMode>
      <I18nProvider>
        <App serial={serial} />
      </I18nProvider>
    </StrictMode>,
  )
  await new Promise((r) => setTimeout(r, 50))
  expect(screen.getByText('未連線')).toBeInTheDocument()
  expect(screen.queryByText('已斷線')).not.toBeInTheDocument()
})

test('live-stage overlay classes are namespaced and do not collide with the right-panel tablist', async () => {
  localStorage.setItem('mxcam.demo', '1')
  const { container } = renderApp({ serial: idleSerial })
  await screen.findByText('DEMO')
  const tablist = screen.getByRole('tablist')
  expect(tablist.closest('.stage')).toBeNull()
  expect(tablist.className).toBe('rp-tabs')
  const stage = container.querySelector('.stage')!
  expect(stage.querySelector('.rp-tabs, .tabs, .tab, .box, .boxes, .overlay, .crosshair, .toast')).toBeNull()
  expect(stage.querySelector('.ls-overlay')).not.toBeNull()
  expect(stage.querySelector('.ls-labels')).not.toBeNull()
})

// ---- fix wave ----

test('demo: choosing 人臉 makes faces recognised (the device is told the active function)', async () => {
  localStorage.setItem('mxcam.demo', '1')
  renderApp({ serial: idleSerial })
  await screen.findByText('DEMO')
  fireEvent.click(screen.getByRole('button', { name: '人臉' }))
  expect((await screen.findAllByText(/人臉 \d+%/)).length).toBeGreaterThan(0)
  expect((await screen.findAllByText(/^\d+%$/)).length).toBeGreaterThan(0)
})

test('demo: choosing AprilTag makes Tag 3 appear', async () => {
  localStorage.setItem('mxcam.demo', '1')
  renderApp({ serial: idleSerial })
  await screen.findByText('DEMO')
  fireEvent.click(screen.getByRole('button', { name: 'AprilTag' }))
  expect((await screen.findAllByText('Tag 3')).length).toBeGreaterThan(0)
})

test('real mode while disconnected says 請先連線 MXCam, never 需要更新韌體', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('button', { name: '人臉' })).toHaveAttribute('title', '請先連線 MXCam')
  fireEvent.click(screen.getByRole('tab', { name: '學習' }))
  expect(screen.getAllByText('請先連線 MXCam').length).toBeGreaterThan(0)
  fireEvent.click(screen.getByRole('button', { name: '設定' }))
  expect(screen.getAllByText('請先連線 MXCam').length).toBeGreaterThan(0)
  expect(screen.queryByText('需要更新韌體')).toBeNull()
  expect(screen.getByRole('slider', { name: '亮度' })).toBeDisabled()
})

test('connect-error alert is in normal flow above the columns, can be closed, and never covers the tab bar', async () => {
  const port = {
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    open: async () => {
      throw new DOMException('Failed to open serial port.', 'NetworkError')
    },
    close: async () => {},
  }
  const serial = { ...idleSerial, requestPort: async () => port } as unknown as Serial
  const { container } = renderApp({ serial })
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  const alert = await screen.findByRole('alert')
  const columns = container.querySelector('.app-columns')!
  expect(alert.className).toBe('app-alert')
  expect(columns.contains(alert)).toBe(false)
  expect(alert.nextElementSibling).toBe(columns)
  expect(alert.contains(screen.getByRole('tablist'))).toBe(false)
  fireEvent.click(within(alert).getByRole('button', { name: '關閉' }))
  expect(screen.queryByRole('alert')).not.toBeInTheDocument()
})

function visibleCjk(root: HTMLElement): string[] {
  const found: string[] = []
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT)
  for (let n = walker.nextNode(); n; n = walker.nextNode()) {
    const el = n.parentElement
    if (el?.closest('[lang="zh-Hant"]')) continue
    if (/[一-鿿]/.test(n.textContent ?? '')) found.push(`text:${n.textContent}`)
  }
  for (const el of root.querySelectorAll('[aria-label],[title],[placeholder]')) {
    if (el.closest('[lang="zh-Hant"]')) continue
    for (const a of ['aria-label', 'title', 'placeholder']) {
      const v = el.getAttribute(a)
      if (v && /[一-鿿]/.test(v)) found.push(`${a}:${v}`)
    }
  }
  return found
}

test('English demo mode leaks no CJK text or attributes on any tab of any view', async () => {
  localStorage.setItem('mxcam.lang', 'en')
  localStorage.setItem('mxcam.demo', '1')
  const { container } = renderApp({ serial: idleSerial })
  await screen.findByText('DEMO')
  const leaks: string[] = []
  for (const fn of ['Color', 'AprilTag', 'Face']) {
    fireEvent.click(screen.getByRole('button', { name: fn }))
    for (const tabName of ['Results', 'Learn', 'Params', 'Code']) {
      fireEvent.click(screen.getByRole('tab', { name: tabName }))
      leaks.push(...visibleCjk(container).map((x) => `${fn}/${tabName}/${x}`))
    }
  }
  for (const page of ['Settings', 'Firmware']) {
    fireEvent.click(screen.getByRole('button', { name: page }))
    leaks.push(...visibleCjk(container).map((x) => `${page}/${x}`))
  }
  expect(leaks).toEqual([])
})

// ---- M2: function ownership with a real board ----
test('when the R4 changes the function, Studio follows and says so; its own stale frames are ignored', async () => {
  const fw = new FakeFirmware()
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  await waitFor(() => expect(fw.requests.some((r) => r.cmd === Cmd.SetFunction && r.payload[0] === 1)).toBe(true))
  await waitFor(() => expect(fw.active).toBe(1))
  fw.pushResults(1, 2, []) // in flight from before our SET_FUNCTION
  await new Promise((r) => setTimeout(r, 50))
  expect(screen.getByRole('button', { name: '顏色' })).toHaveAttribute('aria-current', 'page')
  expect(screen.queryByText(/由其他裝置變更/)).toBeNull()
  await new Promise((r) => setTimeout(r, 1100))
  fw.externalSetFunction(2)
  fw.pushResults(2, 2, [{ id: 3, x: 100, y: 100, width: 40, height: 40 }])
  expect(await screen.findByText('已切換到 AprilTag（由其他裝置變更）')).toBeInTheDocument()
  expect(screen.getByRole('button', { name: 'AprilTag' })).toHaveAttribute('aria-current', 'page')
  expect((await screen.findAllByText('Tag 3')).length).toBeGreaterThan(0)
  // the board already runs AprilTag: no echo SET_FUNCTION(2)
  await new Promise((r) => setTimeout(r, 50))
  expect(fw.requests.filter((r) => r.cmd === Cmd.SetFunction && r.payload[0] === 2)).toHaveLength(0)
})

test('when another device switches the board to function 0, the page stays and a notice says so', async () => {
  const fw = new FakeFirmware()
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  await waitFor(() => expect(fw.active).toBe(1))
  await new Promise((r) => setTimeout(r, 1100))
  fw.externalSetFunction(0)
  fw.pushResults(2, 0, [])
  expect(await screen.findByText('其他裝置把功能切換成「無功能」')).toBeInTheDocument()
  expect(screen.getByRole('button', { name: '顏色' })).toHaveAttribute('aria-current', 'page')
  await new Promise((r) => setTimeout(r, 50))
  expect(fw.requests.filter((r) => r.cmd === Cmd.SetFunction)).toHaveLength(1) // no re-send loop
})

test('SET_FUNCTION failing is retried once after 500 ms, then reported', async () => {
  const fw = new FakeFirmware()
  fw.failSetFunction = 2
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  const alert = await screen.findByRole('alert', undefined, { timeout: 3000 })
  expect(alert).toHaveTextContent('切換功能失敗')
  expect(fw.requests.filter((r) => r.cmd === Cmd.SetFunction)).toHaveLength(2)
})

test('SET_FUNCTION failing once and succeeding on the retry shows nothing', async () => {
  const fw = new FakeFirmware()
  fw.failSetFunction = 1
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  await waitFor(() => expect(fw.active).toBe(1), { timeout: 3000 })
  expect(screen.queryByRole('alert')).toBeNull()
})

test('real board: results pushes show up in the results tab', async () => {
  const fw = new FakeFirmware()
  fw.colors = [{ id: 1, rgb: [255, 0, 0] }]
  fw.names.set('1:1', '紅色積木')
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  fw.pushResults(5, 1, [{ id: 1, x: 160, y: 120, width: 30, height: 20 }])
  expect((await screen.findAllByText('紅色積木')).length).toBeGreaterThan(0)
  expect(screen.getAllByText('ID1').length).toBeGreaterThan(0)
})

test('real board: face is selectable when GET_INFO lists 3, and results read 人臉 92%', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  const face = screen.getByRole('button', { name: '人臉' })
  expect(face).not.toHaveAttribute('aria-disabled')
  fireEvent.click(face)
  await waitFor(() => expect(fw.active).toBe(3))
  fw.pushResults(5, 3, [{ id: 0, x: 160, y: 120, width: 50, height: 60, confidence: 92 }])
  expect((await screen.findAllByText('92%')).length).toBeGreaterThan(0)
})

test('real board: face stays greyed with 需要更新韌體 when GET_INFO lacks 3', async () => {
  const fw = new FakeFirmware()
  renderApp({ serial: fw.serial() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  const face = screen.getByRole('button', { name: '人臉' })
  expect(face).toHaveAttribute('aria-disabled', 'true')
  expect(face).toHaveAttribute('title', '需要更新韌體')
  fireEvent.click(face)
  expect(fw.requests.some((r) => r.cmd === Cmd.SetFunction && r.payload[0] === 3)).toBe(false)
})

test('demo: the pause-preview toggle pauses and resumes the preview (placeholder shown while paused)', async () => {
  localStorage.setItem('mxcam.demo', '1')
  renderApp({ serial: idleSerial })
  const box = await screen.findByRole('checkbox', { name: '暫停預覽' })
  expect(box).not.toBeChecked()
  fireEvent.click(box)
  expect(await screen.findByText('預覽已暫停（辨識照常進行）')).toBeInTheDocument()
  expect(screen.getByRole('checkbox', { name: '暫停預覽' })).toBeChecked()
  fireEvent.click(screen.getByRole('checkbox', { name: '暫停預覽' }))
  await waitFor(() => expect(screen.queryByText('預覽已暫停（辨識照常進行）')).toBeNull())
})

// ---- Bluetooth next to USB ----
test('with both USB and Bluetooth there is one connect button for each', () => {
  const fw = new FakeFirmware()
  renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  expect(screen.getByRole('button', { name: 'USB 連線' })).toBeEnabled()
  expect(screen.getByRole('button', { name: '藍牙連線' })).toBeEnabled()
  expect(screen.queryByRole('button', { name: '連線' })).not.toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: 'English' }))
  expect(screen.getByRole('button', { name: 'Connect USB' })).toBeInTheDocument()
  expect(screen.getByRole('button', { name: 'Connect Bluetooth' })).toBeInTheDocument()
})

test('with USB only there is no Bluetooth button', () => {
  renderApp({ serial: idleSerial })
  expect(screen.getByRole('button', { name: '連線' })).toBeEnabled()
  expect(screen.queryByRole('button', { name: /藍牙/ })).not.toBeInTheDocument()
})

test('with Bluetooth only the browser is supported and the single button connects over Bluetooth', async () => {
  const fw = new FakeFirmware()
  renderApp({ bluetooth: fw.bluetooth() })
  expect(screen.queryByText('此瀏覽器不支援，請用 Chrome 或 Edge')).not.toBeInTheDocument()
  expect(screen.queryByRole('button', { name: /USB/ })).not.toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '連線' }))
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  expect(fw.btConnected).toBe(true)
})

test('demo mode hides both connect buttons', async () => {
  localStorage.setItem('mxcam.demo', '1')
  const fw = new FakeFirmware()
  renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  expect(await screen.findByText('DEMO')).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: /連線/ })).not.toBeInTheDocument()
})

test('connecting over Bluetooth says so; a drop shows 已斷線 and how to reconnect; disconnect works', async () => {
  const fw = new FakeFirmware()
  const { container } = renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  fireEvent.click(screen.getByRole('button', { name: '藍牙連線' }))
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  expect(container.querySelector('.conn-status .tb-via')).toHaveTextContent('藍牙')
  expect(screen.getByRole('button', { name: 'USB 連線' })).toBeDisabled()
  expect(screen.getByRole('button', { name: '藍牙連線' })).toBeDisabled()
  expect(fw.requests.find((r) => r.cmd === Cmd.StreamStart)!.payload[1]).toBe(5)

  fw.btDrop()
  expect(await screen.findByText('已斷線')).toBeInTheDocument()
  expect(screen.getByText('藍牙斷線了，請再按一次「藍牙連線」')).toBeInTheDocument()
  expect(container.querySelector('.tb-via')).toBeNull()

  fireEvent.click(screen.getByRole('button', { name: '藍牙連線' }))
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  expect(screen.queryByText('藍牙斷線了，請再按一次「藍牙連線」')).not.toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '中斷' }))
  expect(await screen.findByText('未連線')).toBeInTheDocument()
  expect(fw.btConnected).toBe(false)
  expect(screen.queryByText('藍牙斷線了，請再按一次「藍牙連線」')).not.toBeInTheDocument()
})

test('a USB connection is labelled USB', async () => {
  const fw = new FakeFirmware()
  const { container } = renderApp({ serial: fw.serial(), bluetooth: fw.bluetooth() })
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  expect(container.querySelector('.conn-status .tb-via')).toHaveTextContent('USB')
})

test('closing the Bluetooth chooser shows no error; Bluetooth being off does', async () => {
  const fw = new FakeFirmware()
  fw.btPicker = 'cancel'
  renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  fireEvent.click(screen.getByRole('button', { name: '藍牙連線' }))
  await new Promise((r) => setTimeout(r, 30))
  expect(screen.queryByRole('alert')).not.toBeInTheDocument()
  expect(screen.getByText('未連線')).toBeInTheDocument()

  fw.btPicker = 'no-adapter'
  fireEvent.click(screen.getByRole('button', { name: '藍牙連線' }))
  expect(await screen.findByRole('alert')).toHaveTextContent('這台電腦的藍牙沒有開，或是沒有藍牙')
})

test('a board whose firmware has no Bluetooth service says so, not a serial-port message', async () => {
  const fw = new FakeFirmware()
  fw.btHasService = false
  renderApp({ serial: idleSerial, bluetooth: fw.bluetooth() })
  fireEvent.click(screen.getByRole('button', { name: '藍牙連線' }))
  expect(await screen.findByRole('alert')).toHaveTextContent('這台 MXCam 的韌體沒有藍牙功能，請改用 USB')
  expect(screen.getByText('未連線')).toBeInTheDocument()
})
