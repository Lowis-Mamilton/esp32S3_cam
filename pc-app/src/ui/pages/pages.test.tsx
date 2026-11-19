import { act, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { useSyncExternalStore } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoDevice } from '../../device/DemoDevice'
import { RealDevice } from '../../device/RealDevice'
import type { Device, FunctionId } from '../../device/types'
import { I18nProvider } from '../../i18n'
import RightPanel from '../layout/RightPanel'
import FirmwarePage from './FirmwarePage'
import SettingsPage from './SettingsPage'

const FULL = { x: 0, y: 0, width: 320, height: 240 }
let devices: Device[] = []

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
})
afterEach(() => {
  devices.forEach((d) => d.dispose())
  devices = []
  vi.restoreAllMocks()
  vi.useRealTimers()
})

function useSnap(device: Device) {
  return useSyncExternalStore(
    (cb) => device.subscribe(cb),
    () => device.snapshot(),
  )
}
function SettingsHarness({ device }: { device: Device }) {
  return <SettingsPage device={device} snapshot={useSnap(device)} />
}
function FirmwareHarness({ device }: { device: Device }) {
  return <FirmwarePage snapshot={useSnap(device)} />
}
function PanelHarness({ device, fn }: { device: Device; fn: FunctionId }) {
  return <RightPanel device={device} snapshot={useSnap(device)} fn={fn} results={[]} />
}
function mount(ui: (d: Device) => React.ReactElement, device: Device = new DemoDevice(() => 0)) {
  devices.push(device)
  render(<I18nProvider>{ui(device)}</I18nProvider>)
  return device
}

// ---- settings page ----

test('settings shows DEMO device info', () => {
  mount((d) => <SettingsHarness device={d} />)
  expect(screen.getByText('裝置資訊')).toBeInTheDocument()
  expect(screen.getAllByText(/DEMO/).length).toBeGreaterThan(0)
  expect(screen.getByText('DE:MO:00:00:00:01')).toBeInTheDocument()
  expect(screen.getByText('320x240')).toBeInTheDocument()
})

test('UART baud radio sends setSystem immediately', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  fireEvent.click(screen.getByRole('radio', { name: '9600' }))
  await waitFor(() => expect(spy).toHaveBeenCalledWith({ uartBaud: 9600 }))
  await waitFor(() => expect(d.snapshot().system.uartBaud).toBe(9600))
  expect(screen.getByRole('radio', { name: '9600' })).toBeChecked()
})

test('I2C address shows 0x prefix and a valid value is sent', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  const input = screen.getByRole('textbox', { name: 'I2C 位址' })
  expect(input).toHaveValue('5A')
  expect(screen.getByText('0x')).toBeInTheDocument()
  fireEvent.change(input, { target: { value: '30' } })
  await waitFor(() => expect(spy).toHaveBeenCalledWith({ i2cAddress: 0x30 }))
  expect(screen.queryByRole('alert')).toBeNull()
})

test.each(['FF', '07', '78', 'zz', ''])('I2C address %j is rejected inline and not sent', (v) => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  fireEvent.change(screen.getByRole('textbox', { name: 'I2C 位址' }), { target: { value: v } })
  expect(screen.getByText('I2C 位址必須在 0x08 到 0x77 之間')).toBeInTheDocument()
  expect(spy).not.toHaveBeenCalled()
  expect(d.snapshot().system.i2cAddress).toBe(0x5a)
})

test('LED slider debounces and flushes on unmount', async () => {
  vi.useFakeTimers()
  const device = new DemoDevice(() => 0)
  devices.push(device)
  const spy = vi.spyOn(device, 'setSystem')
  const { unmount } = render(
    <I18nProvider>
      <SettingsHarness device={device} />
    </I18nProvider>,
  )
  const slider = screen.getByRole('slider', { name: '亮度' })
  fireEvent.change(slider, { target: { value: '70' } })
  fireEvent.change(slider, { target: { value: '80' } })
  expect(spy).not.toHaveBeenCalled()
  await act(async () => {
    vi.advanceTimersByTime(200)
  })
  expect(spy).toHaveBeenCalledTimes(1)
  expect(spy).toHaveBeenCalledWith({ ledBrightness: 80 })
  fireEvent.change(slider, { target: { value: '10' } })
  unmount()
  expect(spy).toHaveBeenLastCalledWith({ ledBrightness: 10 })
})

test('factory reset needs inline confirmation, focus goes to cancel', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  await act(async () => {
    await d.setSystem({ ledBrightness: 5 })
  })
  const spy = vi.spyOn(d, 'factoryReset')
  fireEvent.click(screen.getByRole('button', { name: '恢復原廠設定' }))
  expect(spy).not.toHaveBeenCalled()
  expect(screen.getByRole('button', { name: '取消' })).toHaveFocus()
  fireEvent.click(screen.getByRole('button', { name: '取消' }))
  expect(spy).not.toHaveBeenCalled()
  fireEvent.click(screen.getByRole('button', { name: '恢復原廠設定' }))
  fireEvent.click(screen.getByRole('button', { name: '確定恢復' }))
  await waitFor(() => expect(spy).toHaveBeenCalledTimes(1))
  await waitFor(() => expect(d.snapshot().system.ledBrightness).toBe(50))
})

test('real device: change shows 需要更新韌體 and disables controls', async () => {
  mount((x) => <SettingsHarness device={x} />, new RealDevice(undefined))
  fireEvent.click(screen.getByRole('radio', { name: '9600' }))
  expect(await screen.findByText('需要更新韌體')).toBeInTheDocument()
  expect(screen.getByRole('radio', { name: '9600' })).toBeDisabled()
  expect(screen.getByRole('slider', { name: '亮度' })).toBeDisabled()
  expect(screen.getByRole('button', { name: '恢復原廠設定' })).toBeDisabled()
})

// ---- firmware page ----

test('firmware page shows version, chosen file name and 即將推出', () => {
  mount((d) => <FirmwareHarness device={d} />)
  expect(screen.getByText('DEMO')).toBeInTheDocument()
  expect(screen.queryByText('即將推出')).toBeNull()
  const file = new File([new Uint8Array([1, 2])], 'mxcam-1.2.bin')
  fireEvent.change(document.querySelector('input[type=file]')!, { target: { files: [file] } })
  expect(screen.getByText('mxcam-1.2.bin')).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '開始更新' }))
  expect(screen.getByText('即將推出')).toBeInTheDocument()
})

test('firmware choose button opens the file input; input accepts .bin', () => {
  mount((d) => <FirmwareHarness device={d} />)
  const input = document.querySelector('input[type=file]') as HTMLInputElement
  expect(input.accept).toContain('.bin')
  const click = vi.spyOn(input, 'click')
  fireEvent.click(screen.getByRole('button', { name: '選擇韌體檔' }))
  expect(click).toHaveBeenCalled()
})

// ---- export / import in params tab ----

const readBlob = (b: Blob) =>
  new Promise<string>((res, rej) => {
    const r = new FileReader()
    r.onload = () => res(String(r.result))
    r.onerror = () => rej(r.error)
    r.readAsText(b)
  })
const openParams = () => fireEvent.click(screen.getByRole('tab', { name: '參數' }))
const fileInput = () => document.querySelector('input[type=file]') as HTMLInputElement
const upload = (text: string) =>
  fireEvent.change(fileInput(), { target: { files: [new File([text], 'cfg.json', { type: 'application/json' })] } })

test('export downloads mxcam-config.json via a temporary link and revokes the URL', async () => {
  const d = mount((x) => <PanelHarness device={x} fn={1} />)
  await act(async () => {
    await d.learnColor(FULL)
  })
  const blobs: Blob[] = []
  URL.createObjectURL = vi.fn((b: Blob) => (blobs.push(b), 'blob:cfg'))
  URL.revokeObjectURL = vi.fn()
  let download = ''
  vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (this: HTMLAnchorElement) {
    download = this.download
  })
  openParams()
  fireEvent.click(screen.getByRole('button', { name: '匯出' }))
  expect(download).toBe('mxcam-config.json')
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:cfg')
  expect(document.querySelector('a[download]')).toBeNull()
  expect(await readBlob(blobs[0])).toBe(d.exportConfig())
})

test('export then import restores the same config and marks dirty', async () => {
  const d = mount((x) => <PanelHarness device={x} fn={1} />)
  await act(async () => {
    await d.learnColor(FULL)
    await d.setSystem({ uartBaud: 9600, i2cAddress: 0x30 })
    await d.save()
  })
  const exported = d.exportConfig()
  await act(async () => {
    await d.forgetAll(1)
    await d.factoryReset()
  })
  expect(d.exportConfig()).not.toBe(exported)
  openParams()
  upload(exported)
  expect(await screen.findByText('已匯入設定')).toBeInTheDocument()
  expect(d.exportConfig()).toBe(exported)
  expect(d.snapshot().dirty).toBe(true)
  expect(screen.getByText('未儲存')).toBeInTheDocument()
})

test.each(['{not json', '{"version":2}', JSON.stringify({ version: 1, learned: {}, params: {}, system: {} })])(
  'importing bad file %j shows error and leaves state unchanged',
  async (text) => {
    const d = mount((x) => <PanelHarness device={x} fn={1} />)
    const before = d.exportConfig()
    openParams()
    upload(text)
    expect(await screen.findByText(/設定檔格式不正確/)).toBeInTheDocument()
    expect(d.exportConfig()).toBe(before)
    expect(d.snapshot().dirty).toBe(false)
    expect(screen.queryByText('已匯入設定')).toBeNull()
  },
)

test('real device: export works, import says 需要更新韌體', async () => {
  const real = mount((x) => <PanelHarness device={x} fn={1} />, new RealDevice(undefined))
  openParams()
  expect(screen.getByRole('button', { name: '匯出' })).toBeEnabled()
  upload(real.exportConfig())
  expect((await screen.findAllByText('需要更新韌體')).length).toBeGreaterThan(0)
  expect(screen.queryByText(/設定檔格式不正確/)).toBeNull()
})
