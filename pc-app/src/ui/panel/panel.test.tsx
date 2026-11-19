import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { Cmd } from '../../protocol/messages'
import { FakeFirmware } from '../../testing/fakeFirmware'
import { useState, useSyncExternalStore } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoDevice } from '../../device/DemoDevice'
import { RealDevice } from '../../device/RealDevice'
import { ResultsStore } from '../../device/resultsStore'
import { DeviceOpError, type Device, type FunctionId, type Result } from '../../device/types'
import { I18nProvider } from '../../i18n'
import RightPanel from '../layout/RightPanel'
import { snippetFor } from './codeSnippets'

const FULL = { x: 0, y: 0, width: 320, height: 240 }
let devices: Device[] = []

beforeEach(() => {
  try {
    localStorage.clear()
  } catch {
    // ignore
  }
})
const clipboardDesc = Object.getOwnPropertyDescriptor(navigator, 'clipboard')
afterEach(() => {
  if (clipboardDesc) Object.defineProperty(navigator, 'clipboard', clipboardDesc)
  else delete (navigator as unknown as Record<string, unknown>).clipboard
  devices.forEach((d) => d.dispose())
  devices = []
  vi.restoreAllMocks()
})

function Harness({ device, fn, results }: { device: Device; fn: FunctionId; results: Result[] }) {
  const snapshot = useSyncExternalStore(
    (cb) => device.subscribe(cb),
    () => device.snapshot(),
  )
  const [store] = useState(() => {
    const st = new ResultsStore()
    st.push(results)
    return st
  })
  return <RightPanel device={device} snapshot={snapshot} fn={fn} results={store} />
}

function setup(fn: FunctionId = 1, results: Result[] = [], device: Device = new DemoDevice(() => 0)) {
  devices.push(device)
  const utils = render(
    <I18nProvider>
      <Harness device={device} fn={fn} results={results} />
    </I18nProvider>,
  )
  return { device, ...utils }
}
const tab = (name: string) => fireEvent.click(screen.getByRole('tab', { name }))
const save = () => screen.getByRole('button', { name: '存到板子' })

test('initially there is no 未儲存 badge and save is disabled', () => {
  setup()
  expect(screen.queryByText('未儲存')).toBeNull()
  expect(save()).toBeDisabled()
})

test('learning a colour shows 未儲存; saving clears it', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  expect(screen.getByText('未儲存')).toBeInTheDocument()
  expect(save()).toBeEnabled()
  fireEvent.click(save())
  await waitFor(() => expect(screen.queryByText('未儲存')).toBeNull())
  expect(save()).toBeDisabled()
})

test('changing a param shows 未儲存', async () => {
  setup()
  tab('參數')
  fireEvent.click(screen.getByRole('checkbox', { name: '顯示名稱' }))
  await waitFor(() => expect(screen.getByText('未儲存')).toBeInTheDocument())
})

test('slider change is applied to the device after a short debounce and marks dirty', async () => {
  const { device } = setup()
  tab('參數')
  fireEvent.change(screen.getByRole('slider', { name: '容許範圍' }), { target: { value: '55' } })
  await waitFor(() => expect(device.snapshot().params.color.tolerance).toBe(55))
  expect(screen.getByText('未儲存')).toBeInTheDocument()
})

test('failed save is shown inline', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  vi.spyOn(device, 'save').mockRejectedValue(new Error('flash busy'))
  fireEvent.click(save())
  expect(await screen.findByRole('alert')).toHaveTextContent('flash busy')
})

test('results tab lists the active results with integers, and an empty hint', () => {
  const r: Result[] = [{ id: 1, kind: 'color', x: 10.4, y: 20, width: 30, height: 40 }]
  const { unmount } = setup(1, r)
  const row = screen.getByRole('row', { name: /ID1/ })
  expect(within(row).getAllByRole('cell').map((c) => c.textContent)).toEqual(['ID1', '', '10', '20', '30', '40'])
  unmount()
  setup(1, [])
  expect(screen.getByText('目前沒有辨識結果')).toBeInTheDocument()
})

test('results tab hints that coordinates are true-image ones while the mirror view is on', () => {
  const r: Result[] = [{ id: 1, kind: 'color', x: 10, y: 20, width: 30, height: 40 }]
  localStorage.setItem('mxcam.mirror', '1')
  const { unmount } = setup(1, r)
  expect(screen.getByText('座標是鏡頭真實影像的座標（R4 收到的數字）')).toBeInTheDocument()
  unmount()
  localStorage.setItem('mxcam.mirror', '0')
  setup(1, r)
  expect(screen.queryByText(/座標是鏡頭真實影像/)).toBeNull()
  localStorage.removeItem('mxcam.mirror')
})

test('face results show a confidence column', () => {
  setup(3, [{ id: 0, kind: 'face', x: 1, y: 2, width: 3, height: 4, confidence: 87.6 }])
  expect(screen.getByRole('columnheader', { name: '信心度' })).toBeInTheDocument()
  expect(screen.getByText('88%')).toBeInTheDocument()
})

test('renaming a learned colour shows the new name in the results tab', async () => {
  const { device } = setup(1, [{ id: 1, kind: 'color', x: 1, y: 2, width: 3, height: 4 }])
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  fireEvent.change(input, { target: { value: '紅積木' } })
  fireEvent.blur(input)
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('紅積木'))
  tab('結果')
  expect(screen.getByRole('row', { name: /ID1/ })).toHaveTextContent('紅積木')
})

test('name input caps characters at 48 (bytes are checked separately) and Enter commits', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  expect(input).toHaveAttribute('maxlength', '48')
  fireEvent.change(input, { target: { value: 'abc' } })
  fireEvent.keyDown(input, { key: 'Enter' })
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('abc'))
})

test('delete forgets a single ID', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  fireEvent.click(screen.getByRole('button', { name: '刪除 ID1' }))
  await waitFor(() => expect(device.snapshot().learned[1]).toHaveLength(0))
})

test('全部忘記 needs an inline confirmation; cancel does nothing', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  fireEvent.click(screen.getByRole('button', { name: '全部忘記' }))
  expect(device.snapshot().learned[1]).toHaveLength(1)
  fireEvent.click(screen.getByRole('button', { name: '取消' }))
  expect(device.snapshot().learned[1]).toHaveLength(1)
  fireEvent.click(screen.getByRole('button', { name: '全部忘記' }))
  fireEvent.click(screen.getByRole('button', { name: '確定忘記' }))
  await waitFor(() => expect(device.snapshot().learned[1]).toHaveLength(0))
})

test('AprilTag: name a tag id, list it, delete it; id range is validated', async () => {
  const { device } = setup(2)
  tab('學習')
  fireEvent.change(screen.getByRole('spinbutton', { name: 'Tag 編號' }), { target: { value: '587' } })
  fireEvent.change(screen.getByRole('textbox', { name: '名稱' }), { target: { value: '終點' } })
  expect(screen.getByRole('button', { name: '加入' })).toBeDisabled()
  fireEvent.change(screen.getByRole('spinbutton', { name: 'Tag 編號' }), { target: { value: '12' } })
  fireEvent.click(screen.getByRole('button', { name: '加入' }))
  await waitFor(() => expect(device.snapshot().learned[2]).toEqual([{ id: 12, name: '終點' }]))
  expect(screen.getByText('Tag 12')).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '刪除 Tag 12' }))
  await waitFor(() => expect(device.snapshot().learned[2]).toHaveLength(0))
})

test('face learn tab says no learning is needed', () => {
  setup(3)
  tab('學習')
  expect(screen.getByText('人臉偵測不需要學習')).toBeInTheDocument()
})


test('params: tag family is read-only tag36h11; face has threshold', () => {
  const { unmount } = setup(2)
  tab('參數')
  expect(screen.getByText('tag36h11')).toBeInTheDocument()
  unmount()
  setup(3)
  tab('參數')
  expect(screen.getByRole('slider', { name: '偵測門檻' })).toBeInTheDocument()
  expect(screen.getByRole('slider', { name: '最多結果數' })).toHaveAttribute('max', '8')
})

test('snippetFor includes the API for every function; comments follow the language', () => {
  for (const fn of [1, 2, 3] as FunctionId[]) {
    for (const lang of ['zh-TW', 'en'] as const) {
      const s = snippetFor(fn, lang)
      expect(s).toContain('#include <MXCam.h>')
      expect(s).toContain('MXCam cam;')
      expect(s).toContain('cam.begin(Serial1);')
      expect(s).toContain('cam.setAlgorithm(')
      expect(s).toContain('cam.request()')
      expect(s).toContain('cam.read()')
    }
  }
  expect(snippetFor(1, 'en')).toContain('MXCAM_ALGO_COLOR')
  expect(snippetFor(2, 'en')).toContain('MXCAM_ALGO_TAG')
  expect(snippetFor(3, 'en')).toContain('MXCAM_ALGO_FACE')
  expect(snippetFor(1, 'en')).not.toMatch(/[一-鿿]/)
  expect(snippetFor(1, 'zh-TW')).toMatch(/[一-鿿]/)
})

test('code tab copies the snippet and confirms; works without clipboard', async () => {
  const writeText = vi.fn().mockResolvedValue(undefined)
  Object.defineProperty(navigator, 'clipboard', { value: { writeText }, configurable: true })
  setup(1)
  tab('程式')
  fireEvent.click(screen.getByRole('button', { name: '複製' }))
  expect(writeText).toHaveBeenCalledWith(snippetFor(1, 'zh-TW'))
  expect(await screen.findByText('已複製')).toBeInTheDocument()
  Object.defineProperty(navigator, 'clipboard', { value: undefined, configurable: true })
  fireEvent.click(screen.getByRole('button', { name: '複製' }))
})

test('real device, disconnected: hints say 請先連線 MXCam (not the firmware hint) and controls are disabled', () => {
  setup(1, [], new RealDevice(undefined))
  tab('學習')
  expect(screen.getAllByText('請先連線 MXCam').length).toBeGreaterThan(0)
  expect(screen.queryByText('需要更新韌體')).toBeNull()
  expect(screen.getByRole('button', { name: '全部忘記' })).toBeDisabled()
  tab('參數')
  expect(screen.getByRole('slider', { name: '容許範圍' })).toBeDisabled()
  expect(save()).toBeDisabled()
})

test('real device, connected without the function: controls are disabled with a firmware hint, nothing throws', () => {
  const real = new RealDevice(undefined)
  vi.spyOn(real, 'snapshot').mockReturnValue({ ...real.snapshot(), status: 'connected', supported: [] })
  setup(1, [], real)
  tab('學習')
  expect(screen.getAllByText('需要更新韌體').length).toBeGreaterThan(0)
  expect(screen.getByRole('button', { name: '全部忘記' })).toBeDisabled()
  tab('參數')
  expect(screen.getByRole('slider', { name: '容許範圍' })).toBeDisabled()
  expect(save()).toBeDisabled()
})

test('NotSupportedError from the device is caught and shown as a firmware hint', async () => {
  const { device } = setup()
  vi.spyOn(device, 'setParams').mockRejectedValue(Object.assign(new Error('x'), { name: 'NotSupportedError' }))
  tab('參數')
  fireEvent.click(screen.getByRole('checkbox', { name: '顯示名稱' }))
  expect(await screen.findByText('需要更新韌體')).toBeInTheDocument()
})

// ---- review fixes ----

test('SaveBar keeps its DOM node when dirty changes, and a failed save error clears when dirty changes', async () => {
  const { device, container } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  const bar = container.querySelector('.rp-savebar')!
  vi.spyOn(device, 'save').mockRejectedValueOnce(new Error('disk full'))
  fireEvent.click(save())
  expect(await screen.findByText('disk full')).toBeInTheDocument()
  await act(async () => {
    await device.factoryReset() // dirty true -> false
  })
  expect(screen.queryByText('disk full')).toBeNull()
  expect(container.querySelector('.rp-savebar')).toBe(bar)
})


test('a slider change is flushed when the tab is left within the debounce window', async () => {
  const { device } = setup()
  const spy = vi.spyOn(device, 'setParams')
  tab('參數')
  fireEvent.change(screen.getByRole('slider', { name: '容許範圍' }), { target: { value: '70' } })
  tab('結果')
  await waitFor(() => expect(spy).toHaveBeenCalled())
  expect(device.snapshot().params.color.tolerance).toBe(70)
  await waitFor(() => expect(screen.getByText('未儲存')).toBeInTheDocument())
})

test('Enter that confirms an IME composition does not commit the name', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  fireEvent.change(input, { target: { value: '紅色' } })
  fireEvent.keyDown(input, { key: 'Enter', isComposing: true })
  fireEvent.keyDown(input, { key: 'Process', keyCode: 229 })
  await act(async () => {})
  expect(device.snapshot().learned[1][0].name).toBe('')
  fireEvent.keyDown(input, { key: 'Enter' })
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('紅色'))
})

test('name input shows a placeholder', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  expect(screen.getByRole('textbox', { name: 'ID1 名稱' })).toHaveAttribute('placeholder', '未命名')
})

test('tag form: add needs a name, and a failed add keeps the inputs', async () => {
  const { device } = setup(2)
  tab('學習')
  const num = screen.getByRole('spinbutton', { name: 'Tag 編號' })
  const nm = screen.getByRole('textbox', { name: '名稱' })
  fireEvent.change(num, { target: { value: '5' } })
  expect(screen.getByRole('button', { name: '加入' })).toBeDisabled()
  fireEvent.change(nm, { target: { value: '門' } })
  vi.spyOn(device, 'setName').mockRejectedValue(new Error('nope'))
  fireEvent.click(screen.getByRole('button', { name: '加入' }))
  expect(await screen.findByRole('alert')).toHaveTextContent('操作失敗')
  expect(num).toHaveValue(5)
  expect(nm).toHaveValue('門')
})

test('panel errors are localized, carry a small detail, and clear on tab change', async () => {
  const { device } = setup()
  vi.spyOn(device, 'setParams').mockRejectedValue(new Error('boom'))
  tab('參數')
  fireEvent.click(screen.getByRole('checkbox', { name: '顯示名稱' }))
  const alert = await screen.findByRole('alert')
  expect(alert).toHaveTextContent('操作失敗')
  expect(alert).toHaveTextContent('boom')
  tab('學習')
  expect(screen.queryByRole('alert')).toBeNull()
})

test('save error is localized and cleared by the next attempt', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  const spy = vi.spyOn(device, 'save').mockRejectedValueOnce(new Error('flash busy'))
  fireEvent.click(save())
  const alert = await screen.findByRole('alert')
  expect(alert).toHaveTextContent('儲存失敗')
  expect(alert).toHaveTextContent('flash busy')
  fireEvent.click(save())
  await waitFor(() => expect(screen.queryByRole('alert')).toBeNull())
  expect(spy).toHaveBeenCalledTimes(2)
})

test('save error disappears when the dirty state changes', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  vi.spyOn(device, 'save').mockRejectedValueOnce(new Error('x'))
  fireEvent.click(save())
  await screen.findByRole('alert')
  await act(async () => {
    await device.factoryReset()
  })
  expect(screen.queryByRole('alert')).toBeNull()
})

test('snippets show exactly one algorithm, no combined flags', () => {
  for (const fn of [1, 2, 3] as FunctionId[])
    for (const lang of ['zh-TW', 'en'] as const) {
      const s = snippetFor(fn, lang)
      expect(s).not.toContain('|')
      expect(s.match(/setAlgorithm/g)).toHaveLength(1)
    }
})

test('copy failure is reported', async () => {
  Object.defineProperty(navigator, 'clipboard', { value: { writeText: vi.fn().mockRejectedValue(new Error('denied')) }, configurable: true })
  setup(1)
  tab('程式')
  fireEvent.click(screen.getByRole('button', { name: '複製' }))
  expect(await screen.findByText('複製失敗，請手動選取')).toBeInTheDocument()
})

test('copy without clipboard also reports failure', async () => {
  Object.defineProperty(navigator, 'clipboard', { value: undefined, configurable: true })
  setup(1)
  tab('程式')
  fireEvent.click(screen.getByRole('button', { name: '複製' }))
  expect(await screen.findByText('複製失敗，請手動選取')).toBeInTheDocument()
})

test('opening the forget-all confirm moves focus to Cancel', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  fireEvent.click(screen.getByRole('button', { name: '全部忘記' }))
  await waitFor(() => expect(screen.getByRole('button', { name: '取消' })).toHaveFocus())
})

// ---- M2: names are limited in UTF-8 bytes ----
test('name inputs limit names to 48 UTF-8 bytes, not characters, and say so', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learn(1, FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  fireEvent.change(input, { target: { value: '中'.repeat(20) } })
  expect(input).toHaveValue('中'.repeat(16))
  expect(screen.getByText('名稱最多 48 bytes（中文約 16 字）')).toBeInTheDocument()
  fireEvent.blur(input)
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('中'.repeat(16)))
  fireEvent.change(input, { target: { value: 'a'.repeat(60) } })
  expect(input).toHaveValue('a'.repeat(48))
})

test('tag name input is limited to 48 UTF-8 bytes', () => {
  setup(2)
  tab('學習')
  const nm = screen.getByRole('textbox', { name: '名稱' })
  fireEvent.change(nm, { target: { value: '起點'.repeat(10) } })
  expect(nm).toHaveValue('起點'.repeat(8))
})

test('a full name table on the device shows a localized message', async () => {
  const { device } = setup(2)
  vi.spyOn(device, 'setName').mockRejectedValue(new DeviceOpError('names-full', 'name table full'))
  tab('學習')
  fireEvent.change(screen.getByRole('spinbutton'), { target: { value: '4' } })
  fireEvent.change(screen.getByRole('textbox', { name: '名稱' }), { target: { value: 'x' } })
  fireEvent.click(screen.getByRole('button', { name: '加入' }))
  expect(await screen.findByText('名字已達上限（最多 32 個）')).toBeInTheDocument()
})

test('face threshold has a hint that values below 50 find no more faces (zh-TW)', () => {
  setup(3)
  tab('參數')
  expect(screen.getByText('門檻低於 50 不會多找到臉')).toBeInTheDocument()
  expect(screen.getByRole('slider', { name: '偵測門檻' })).toHaveAccessibleDescription('門檻低於 50 不會多找到臉')
})

test('face on a real board: params go through SET_CONFIG, the learn tab has no buttons, LEARN/SET_NAME are never sent', async () => {
  const fw = new FakeFirmware()
  fw.functionIds = [1, 2, 3]
  const d = new RealDevice(fw.serial())
  await waitFor(() => expect(d.snapshot().status).toBe('connected'))
  setup(3, [], d)
  tab('參數')
  fireEvent.change(screen.getByRole('slider', { name: '偵測門檻' }), { target: { value: '70' } })
  await waitFor(() => expect(fw.config.faceThreshold).toBe(70))
  await waitFor(() => expect(screen.getByText('未儲存')).toBeInTheDocument())
  tab('學習')
  expect(screen.getByText('人臉偵測不需要學習')).toBeInTheDocument()
  expect(screen.queryAllByRole('button').filter((b) => /學習|加入|刪除/.test(b.textContent ?? b.getAttribute('aria-label') ?? ''))).toHaveLength(0)
  expect(fw.cmds()).not.toContain(Cmd.Learn)
  expect(fw.cmds()).not.toContain(Cmd.SetName)
})

test('face code tab uses MXCAM_ALGO_FACE and reads the confidence', () => {
  setup(3)
  tab('程式')
  expect(document.body.textContent).toContain('MXCAM_ALGO_FACE')
  expect(snippetFor(3, 'en')).toContain('r.confidence')
  expect(snippetFor(1, 'en')).not.toContain('r.confidence')
})

const gestureResult: Result = { id: 7, kind: 'gesture', confidence: 88, x: 100, y: 100, width: 40, height: 40 }

test('gesture results tab has a gesture column and confidence', () => {
  setup(8, [gestureResult])
  expect(screen.getByRole('columnheader', { name: '手勢' })).toBeInTheDocument()
  expect(screen.getByRole('cell', { name: 'OK' })).toBeInTheDocument()
  expect(screen.getByText('88%')).toBeInTheDocument()
})

test('gesture learn tab: no learning, nine rows, naming sends setName(8, id, name)', async () => {
  const { device } = setup(8)
  const spy = vi.spyOn(device, 'setName')
  tab('學習')
  expect(screen.getByText(/手勢辨識不需要學習/)).toBeInTheDocument()
  expect(screen.getAllByRole('textbox')).toHaveLength(9)
  const input = screen.getByRole('textbox', { name: 'OK 名稱' })
  fireEvent.change(input, { target: { value: '前進' } })
  fireEvent.blur(input)
  await waitFor(() => expect(spy).toHaveBeenCalledWith(8, 7, '前進'))
  fireEvent.click(await screen.findByRole('button', { name: '刪除 OK' }))
  await waitFor(() => expect(device.snapshot().learned[8]).toHaveLength(0))
})

test('gesture params: max hands 2 sends the whole gesture config', async () => {
  const { device } = setup(8)
  const spy = vi.spyOn(device, 'setParams')
  tab('參數')
  expect(screen.getByRole('slider', { name: '偵測到手的門檻' })).toBeInTheDocument()
  expect(screen.getByRole('slider', { name: '手勢門檻' })).toBeInTheDocument()
  fireEvent.click(screen.getByRole('button', { name: '2' }))
  await waitFor(() => expect(spy).toHaveBeenCalled())
  expect(spy.mock.calls[0][0].gesture).toEqual({ ...device.snapshot().params.gesture, maxHands: 2 })
  expect(device.snapshot().params.gesture.maxHands).toBe(2)
})

test('gesture code tab uses MXCAM_ALGO_GESTURE', () => {
  setup(8)
  tab('程式')
  expect(document.body.textContent).toContain('MXCAM_ALGO_GESTURE')
  expect(snippetFor(8, 'en')).toContain('MXCAM_GESTURE_LIKE')
})
