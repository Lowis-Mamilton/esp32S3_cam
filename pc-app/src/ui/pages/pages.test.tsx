import { act, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { useSyncExternalStore } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoDevice } from '../../device/DemoDevice'
import { RealDevice } from '../../device/RealDevice'
import { ResultsStore } from '../../device/resultsStore'
import { Cmd, ErrorCode } from '../../protocol/messages'
import { FakeFirmware } from '../../testing/fakeFirmware'
import type { Device, FunctionId } from '../../device/types'
import { I18nProvider } from '../../i18n'
import RightPanel from '../layout/RightPanel'
import FirmwarePage from './FirmwarePage'
import SettingsPage from './SettingsPage'

const FULL = { x: 0, y: 0, width: 320, height: 240 }
const EMPTY_RESULTS = new ResultsStore()
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
  return <RightPanel device={device} snapshot={useSnap(device)} fn={fn} results={EMPTY_RESULTS} />
}
/** A RealDevice connected to older firmware without the config commands (they answer UNKNOWN_CMD). */
async function connectedReal(supported: FunctionId[] = []) {
  const fw = new FakeFirmware()
  fw.functionIds = supported
  for (const c of [Cmd.GetConfig, Cmd.SetConfig, Cmd.GetLearned, Cmd.Save, Cmd.FactoryReset]) fw.unknown.add(c)
  const r = new RealDevice(fw.serial())
  await waitFor(() => expect(r.snapshot().status).toBe('connected'))
  return r
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

const i2c = () => screen.getByRole('textbox', { name: 'I2C 位址' })
const RANGE = 'I2C 位址必須在 0x08 到 0x77 之間'
const type = (v: string) => fireEvent.change(i2c(), { target: { value: v } })

test('I2C address shows 0x prefix; typing sends nothing until Enter', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  expect(i2c()).toHaveValue('5A')
  expect(screen.getByText('0x')).toBeInTheDocument()
  type('A')
  type('A1')
  expect(spy).not.toHaveBeenCalled()
  expect(screen.queryByText(RANGE)).toBeNull()
  fireEvent.keyDown(i2c(), { key: 'Enter' })
  expect(screen.getByText(RANGE)).toBeInTheDocument()
  expect(spy).not.toHaveBeenCalled()
  expect(d.snapshot().system.i2cAddress).toBe(0x5a)
})

test('I2C Enter sends the value and clears the draft', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  type('30')
  fireEvent.keyDown(i2c(), { key: 'Enter' })
  await waitFor(() => expect(spy).toHaveBeenCalledWith({ i2cAddress: 0x30 }))
  expect(screen.queryByText(RANGE)).toBeNull()
  // device-side change (factory reset) shows because the draft was cleared
  await act(async () => {
    await d.factoryReset()
  })
  expect(i2c()).toHaveValue('5A')
})

test('I2C "0x3C" + blur sends 0x3C', async () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  type('0x3C')
  fireEvent.blur(i2c())
  await waitFor(() => expect(spy).toHaveBeenCalledWith({ i2cAddress: 0x3c }))
})

test.each(['FF', '07', '78', 'zz', ''])('I2C %j is rejected on Enter and not sent', (v) => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  type(v)
  fireEvent.keyDown(i2c(), { key: 'Enter' })
  expect(screen.getByText(RANGE)).toBeInTheDocument()
  expect(spy).not.toHaveBeenCalled()
})

test('I2C invalid on blur keeps error and draft; Escape reverts', () => {
  const d = mount((x) => <SettingsHarness device={x} />)
  const spy = vi.spyOn(d, 'setSystem')
  type('FF')
  fireEvent.blur(i2c())
  expect(screen.getByText(RANGE)).toBeInTheDocument()
  expect(i2c()).toHaveValue('FF')
  fireEvent.keyDown(i2c(), { key: 'Escape' })
  expect(i2c()).toHaveValue('5A')
  expect(screen.queryByText(RANGE)).toBeNull()
  expect(spy).not.toHaveBeenCalled()
})

test('real device: I2C commit shows 需要更新韌體 and disables the field', async () => {
  mount((x) => <SettingsHarness device={x} />, await connectedReal())
  type('30')
  fireEvent.keyDown(i2c(), { key: 'Enter' })
  expect(await screen.findByText('需要更新韌體')).toBeInTheDocument()
  expect(i2c()).toBeDisabled()
  expect(i2c()).toHaveValue('—')
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
  expect(screen.getByRole('button', { name: '恢復原廠設定' })).toHaveFocus()
  fireEvent.click(screen.getByRole('button', { name: '恢復原廠設定' }))
  fireEvent.click(screen.getByRole('button', { name: '確定恢復' }))
  await waitFor(() => expect(spy).toHaveBeenCalledTimes(1))
  await waitFor(() => expect(d.snapshot().system.ledBrightness).toBe(50))
})

test('real device: change shows 需要更新韌體 and disables controls', async () => {
  mount((x) => <SettingsHarness device={x} />, await connectedReal())
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
  vi.useFakeTimers()
  fireEvent.click(screen.getByRole('button', { name: '匯出' }))
  expect(download).toBe('mxcam-config.json')
  expect(URL.revokeObjectURL).not.toHaveBeenCalled() // revoked after the browser has started the download
  vi.advanceTimersByTime(1500)
  expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:cfg')
  vi.useRealTimers()
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

test('real device, disconnected: export and import are disabled with 請先連線 MXCam, not the firmware hint', () => {
  mount((x) => <PanelHarness device={x} fn={1} />, new RealDevice(undefined))
  openParams()
  expect(screen.getByRole('button', { name: '匯出' })).toBeDisabled()
  expect(screen.getByRole('button', { name: '匯入' })).toBeDisabled()
  expect(screen.getAllByText('請先連線 MXCam').length).toBeGreaterThan(0)
  expect(screen.queryByText('需要更新韌體')).toBeNull()
})

test('real device exportConfig throws NotConnectedError while disconnected', () => {
  const r = new RealDevice(undefined)
  devices.push(r)
  expect(() => r.exportConfig()).toThrow(expect.objectContaining({ name: 'NotConnectedError' }))
})

test('real device, connected: export shows 需要更新韌體 and never downloads made-up defaults', async () => {
  const create = vi.fn()
  Object.defineProperty(URL, 'createObjectURL', { value: create, configurable: true })
  mount((x) => <PanelHarness device={x} fn={1} />, await connectedReal([1]))
  openParams()
  fireEvent.click(screen.getByRole('button', { name: '匯出' }))
  expect((await screen.findAllByText('需要更新韌體')).length).toBeGreaterThan(0)
  expect(create).not.toHaveBeenCalled()
  delete (URL as unknown as Record<string, unknown>).createObjectURL
})

test('real device settings show — instead of default system values and 請先連線 when disconnected', () => {
  mount((x) => <SettingsHarness device={x} />, new RealDevice(undefined))
  expect(screen.getByText('請先連線 MXCam')).toBeInTheDocument()
  expect(screen.queryByText('需要更新韌體')).toBeNull()
  expect(screen.getByRole('textbox')).toHaveValue('—')
  expect(screen.getByRole('radio', { name: '115200' })).not.toBeChecked()
  expect(screen.getByRole('radio', { name: '115200' })).toBeDisabled()
  expect(screen.getByRole('slider', { name: '亮度' })).toBeDisabled()
  expect(screen.getByText('—', { selector: 'output' })).toBeInTheDocument()
})

test('importing the same file twice works both times', async () => {
  const d = mount((x) => <PanelHarness device={x} fn={1} />)
  const spy = vi.spyOn(d, 'importConfig')
  openParams()
  const text = d.exportConfig()
  upload(text)
  await waitFor(() => expect(spy).toHaveBeenCalledTimes(1))
  expect(fileInput().value).toBe('')
  upload(text)
  await waitFor(() => expect(spy).toHaveBeenCalledTimes(2))
})

test('syntax error gets a short localized detail, not the parser message', async () => {
  mount((x) => <PanelHarness device={x} fn={1} />)
  openParams()
  upload('{not json')
  expect(await screen.findByText('不是有效的 JSON')).toBeInTheDocument()
  expect(screen.queryByText(/invalid config/)).toBeNull()
})

test('oversized import file is rejected without reading it', async () => {
  const d = mount((x) => <PanelHarness device={x} fn={1} />)
  const spy = vi.spyOn(d, 'importConfig')
  openParams()
  upload('x'.repeat(1024 * 1024 + 1))
  expect(await screen.findByText(/設定檔格式不正確/)).toBeInTheDocument()
  expect(screen.getByText('檔案太大（上限 1 MB）')).toBeInTheDocument()
  expect(spy).not.toHaveBeenCalled()
})

// ---- M2: real device import ----
test('importing into a real board applies config and names, and says colours must be re-learned', async () => {
  const fw = new FakeFirmware()
  const r = new RealDevice(fw.serial())
  await waitFor(() => expect(r.snapshot().status).toBe('connected'))
  mount((x) => <PanelHarness device={x} fn={1} />, r)
  const file = {
    version: 1,
    learned: { 1: [{ id: 1, name: '紅', color: [255, 0, 0] }], 2: [{ id: 3, name: '起點' }], 3: [] },
    params: { ...r.snapshot().params, color: { ...r.snapshot().params.color, tolerance: 60 } },
    system: r.snapshot().system,
  }
  openParams()
  upload(JSON.stringify(file))
  expect(await screen.findByText('已匯入設定')).toBeInTheDocument()
  expect(screen.getByText('顏色需要重新學習')).toBeInTheDocument()
  expect(r.snapshot().params.color.tolerance).toBe(60)
  expect(r.snapshot().learned[2]).toEqual([{ id: 3, name: '起點' }])
  expect(fw.requests.some((q) => q.cmd === Cmd.Learn)).toBe(false)
})

test('real board settings show the device values once connected', async () => {
  const fw = new FakeFirmware()
  fw.config = { ...fw.config, i2cAddress: 0x31, uartBaud: 9600, ledBrightness: 12 }
  const r = new RealDevice(fw.serial())
  await waitFor(() => expect(r.snapshot().status).toBe('connected'))
  mount((x) => <SettingsHarness device={x} />, r)
  expect(i2c()).toHaveValue('31')
  expect(screen.getByRole('radio', { name: '9600' })).toBeChecked()
  expect(screen.getByRole('slider', { name: '亮度' })).toHaveValue('12')
})

// ---- review fix round ----
test('a device error during import is an operation failure with a localized reason, not a bad file', async () => {
  const fw = new FakeFirmware()
  const r = new RealDevice(fw.serial())
  await waitFor(() => expect(r.snapshot().status).toBe('connected'))
  mount((x) => <PanelHarness device={x} fn={1} />, r)
  fw.failWith.set(Cmd.SetName, ErrorCode.Full)
  const file = JSON.parse(r.exportConfig())
  file.learned[2] = [{ id: 3, name: '起點' }]
  openParams()
  upload(JSON.stringify(file))
  expect(await screen.findByText('名字已達上限（最多 32 個）')).toBeInTheDocument()
  expect(screen.getByText('操作失敗', { exact: false })).toBeInTheDocument()
  expect(screen.getByText('板子上的設定可能只更新了一部分')).toBeInTheDocument()
  expect(screen.queryByText(/設定檔格式不正確/)).toBeNull()
  expect(screen.queryByText('已匯入設定')).toBeNull()
})

test('older firmware without config: settings and params are disabled up front with 需要更新韌體', async () => {
  const r = await connectedReal([1])
  mount((x) => <SettingsHarness device={x} />, r)
  expect(screen.getByText('需要更新韌體')).toBeInTheDocument()
  expect(screen.getByRole('radio', { name: '9600' })).toBeDisabled()
  expect(screen.getByRole('slider', { name: '亮度' })).toBeDisabled()
  expect(screen.getByRole('button', { name: '恢復原廠設定' })).toBeDisabled()
})

test('older firmware without config: params tab is disabled up front', async () => {
  mount((x) => <PanelHarness device={x} fn={1} />, await connectedReal([1]))
  openParams()
  expect(screen.getAllByText('需要更新韌體').length).toBeGreaterThan(0)
  expect(screen.getByRole('slider', { name: '容許範圍' })).toBeDisabled()
  expect(screen.getByRole('button', { name: '匯出' })).toBeDisabled()
})
