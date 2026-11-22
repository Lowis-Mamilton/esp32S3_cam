import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { Cmd } from '../../protocol/messages'
import { FakeFirmware } from '../../testing/fakeFirmware'
import { useState, useSyncExternalStore } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoDevice } from '../../device/DemoDevice'
import { RealDevice } from '../../device/RealDevice'
import { ResultsStore } from '../../device/resultsStore'
import { DeviceOpError, emptyLearned, type Device, type FunctionId, type LearnedId, type Result } from '../../device/types'
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

// ---- line tracking (4) and object tracking (5) ----
test('line params: bands slider sends the whole line config; learned colour mode is disabled until learned', async () => {
  const { device } = setup(4)
  const spy = vi.spyOn(device, 'setParams')
  tab('參數')
  expect(screen.getByRole('button', { name: '學到的顏色' })).toBeDisabled()
  expect(screen.queryByRole('slider', { name: '顏色容許度' })).toBeNull()
  fireEvent.change(screen.getByRole('slider', { name: '分帶數' }), { target: { value: '10' } })
  await waitFor(() => expect(spy).toHaveBeenCalled())
  expect(spy.mock.calls[0][0].line).toEqual({ ...device.snapshot().params.line, bands: 10 })
  expect(device.snapshot().params.line.bands).toBe(10)
})

test('line params: automatic threshold toggles 0 and a manual value', async () => {
  const { device } = setup(4)
  tab('參數')
  expect(screen.getByRole('slider', { name: '灰階門檻' })).toBeDisabled()
  fireEvent.click(screen.getByRole('checkbox', { name: '自動' }))
  await waitFor(() => expect(device.snapshot().params.line.threshold).toBe(128))
  expect(screen.getByRole('slider', { name: '灰階門檻' })).toBeEnabled()
})

test('line learn tab: explains the automatic modes; nothing to forget until a colour is learned', () => {
  setup(4)
  tab('學習')
  expect(screen.getByText(/黑線、白線不用學/)).toBeInTheDocument()
  expect(screen.getByRole('button', { name: '忘記（改回自動）' })).toBeDisabled()
})

test('line results tab shows angle, offset and flags', () => {
  setup(4, [{ id: 1, kind: 'line', x: 170, y: 150, width: 21, height: 181, line: { angleDeg: -8, offsetX: 20, start: [180, 240], end: [160, 60], branch: false, cross: true } }])
  expect(screen.getByRole('columnheader', { name: '角度' })).toBeInTheDocument()
  expect(screen.getByRole('cell', { name: '−8°' })).toBeInTheDocument()
  expect(screen.getByRole('cell', { name: '+20' })).toBeInTheDocument()
  expect(screen.getByRole('cell', { name: '路口' })).toBeInTheDocument()
})

test('line code tab reads angle, offset and the crossing flag', () => {
  setup(4)
  tab('程式')
  expect(document.body.textContent).toContain('MXCAM_ALGO_LINE')
  const code = snippetFor(4, 'en')
  expect(code).toContain('r.angle')
  expect(code).toContain('r.offset')
  expect(code).toContain('r.flags & MXCAM_LINE_CROSS')
})

test('track code tab uses MXCAM_ALGO_TRACK and treats an empty answer as lost', () => {
  const code = snippetFor(5, 'zh-TW')
  expect(code).toContain('MXCAM_ALGO_TRACK')
  expect(code).toContain('r.confidence')
  expect(code).toContain('else')
})

test('track results tab: 尚未學習 / 目標遺失 / a row with confidence', async () => {
  const { device, unmount } = setup(5)
  expect(screen.getByText('尚未學習')).toBeInTheDocument()
  await act(async () => {
    await device.setFunction(5)
    await device.learn(5, FULL)
  })
  expect(await screen.findByText('目標遺失')).toBeInTheDocument()
  unmount()
  setup(5, [{ id: 1, kind: 'track', x: 100, y: 90, width: 40, height: 30, confidence: 92 }])
  expect(screen.getByRole('columnheader', { name: '信心度' })).toBeInTheDocument()
  expect(screen.getByText('92%')).toBeInTheDocument()
})

test('track learn tab: shows the learned object, names it and forgets it', async () => {
  const { device } = setup(5)
  tab('學習')
  expect(screen.getByText(/還沒有學習物體/)).toBeInTheDocument()
  expect(screen.getByRole('button', { name: '忘記' })).toBeDisabled()
  await act(async () => {
    await device.setFunction(5)
    await device.learn(5, FULL)
  })
  const spy = vi.spyOn(device, 'setName')
  const input = await screen.findByRole('textbox', { name: '物體 名稱' })
  fireEvent.change(input, { target: { value: '小車' } })
  fireEvent.blur(input)
  await waitFor(() => expect(spy).toHaveBeenCalledWith(5, 1, '小車'))
  fireEvent.click(screen.getByRole('button', { name: '忘記' }))
  await waitFor(() => expect(device.snapshot().learned[5]).toHaveLength(0))
})

test('track params: threshold, template update, reacquire and multi-scale', async () => {
  const { device } = setup(5)
  tab('參數')
  fireEvent.change(screen.getByRole('slider', { name: '比對門檻' }), { target: { value: '70' } })
  await waitFor(() => expect(device.snapshot().params.track.threshold).toBe(70))
  fireEvent.click(screen.getByRole('checkbox', { name: '多尺度搜尋' }))
  await waitFor(() => expect(device.snapshot().params.track.multiScale).toBe(false))
  expect(screen.getByRole('slider', { name: '範本更新' })).toBeInTheDocument()
  expect(screen.getByRole('checkbox', { name: '跟丟時自動找回' })).toBeChecked()
})

// ---- M7: QR code (6), shapes (11), motion (12) ----
const ALL_FNS: FunctionId[] = [1, 2, 3, 4, 5, 6, 8, 11, 12]

test('snippets for every function: the library calls, one algorithm, no combined flags', () => {
  for (const fn of ALL_FNS)
    for (const lang of ['zh-TW', 'en'] as const) {
      const s = snippetFor(fn, lang)
      for (const call of ['#include <MXCam.h>', 'cam.begin(Serial1);', 'cam.request()', 'cam.read()']) expect(s, `${fn} ${lang}`).toContain(call)
      expect(s).not.toContain('|')
      expect(s.match(/setAlgorithm/g)).toHaveLength(1)
      if (lang === 'en') expect(s).not.toMatch(/[一-鿿]/)
    }
})

test('code tab: QR, shapes and motion use their MXCAM_ALGO_* and the matching calls', () => {
  setup(6)
  tab('程式')
  expect(document.body.textContent).toContain('MXCAM_ALGO_QR')
  const qr = snippetFor(6, 'en')
  expect(qr).toContain('cam.readText(i, text, sizeof(text))')
  expect(qr).toContain('r.id')
  const shape = snippetFor(11, 'zh-TW')
  expect(shape).toContain('MXCAM_ALGO_SHAPE')
  expect(shape).toContain('if (r.id == MXCAM_SHAPE_TRIANGLE)')
  const motion = snippetFor(12, 'zh-TW')
  expect(motion).toContain('MXCAM_ALGO_MOTION')
  expect(motion).toContain('if (cam.available())')
  expect(motion).toContain('r.x')
})

test('QR params: nothing to set', () => {
  setup(6)
  tab('參數')
  expect(screen.getByText('QR Code 沒有參數可以調')).toBeInTheDocument()
  expect(screen.queryAllByRole('slider')).toHaveLength(0)
})

test('shape params: min size sends the whole shape config; mode and automatic threshold', async () => {
  const { device } = setup(11)
  const spy = vi.spyOn(device, 'setParams')
  tab('參數')
  expect(screen.getByRole('slider', { name: '最小尺寸' })).toHaveAttribute('min', '16')
  expect(screen.getByRole('slider', { name: '最小尺寸' })).toHaveAttribute('max', '240')
  expect(screen.getByRole('slider', { name: '最多結果數' })).toHaveAttribute('max', '8')
  fireEvent.change(screen.getByRole('slider', { name: '最小尺寸' }), { target: { value: '48' } })
  await waitFor(() => expect(spy).toHaveBeenCalled())
  expect(spy.mock.calls[0][0]).toEqual({ shape: { ...device.snapshot().params.shape, minSize: 48 } })
  expect(device.snapshot().params.shape.minSize).toBe(48)
  // Dark shapes on light paper is the default; the threshold is automatic until unticked.
  expect(screen.getByRole('button', { name: '深色形狀' })).toHaveAttribute('aria-pressed', 'true')
  fireEvent.click(screen.getByRole('button', { name: '淺色形狀' }))
  await waitFor(() => expect(device.snapshot().params.shape.mode).toBe('light'))
  expect(screen.getByRole('slider', { name: '灰階門檻' })).toBeDisabled()
  fireEvent.click(screen.getByRole('checkbox', { name: '自動' }))
  await waitFor(() => expect(device.snapshot().params.shape.threshold).toBe(128))
  expect(screen.getByRole('slider', { name: '灰階門檻' })).toBeEnabled()
})

test('motion params: the threshold sends the whole motion config and says lower is more sensitive', async () => {
  const { device } = setup(12)
  const spy = vi.spyOn(device, 'setParams')
  tab('參數')
  const th = screen.getByRole('slider', { name: '靈敏度門檻' })
  expect(th).toHaveAttribute('min', '5')
  expect(th).toHaveAttribute('max', '100')
  expect(th).toHaveAccessibleDescription('數字越小越靈敏')
  expect(screen.getByRole('slider', { name: '最小尺寸' })).toHaveAttribute('min', '8')
  fireEvent.change(th, { target: { value: '40' } })
  await waitFor(() => expect(spy).toHaveBeenCalled())
  expect(spy.mock.calls[0][0]).toEqual({ motion: { ...device.snapshot().params.motion, threshold: 40 } })
  expect(device.snapshot().params.motion.threshold).toBe(40)
})

function withLearned(device: Device, fn: FunctionId, list: LearnedId[]) {
  const snap = { ...device.snapshot(), learned: { ...emptyLearned(), [fn]: list } }
  vi.spyOn(device, 'snapshot').mockReturnValue(snap)
  return device
}

test('QR learn tab: how to learn, then the learned IDs with their content; naming and forgetting send fn 6', async () => {
  const empty = setup(6)
  tab('學習')
  expect(screen.getByText('在畫面上框住一張 QR Code 就會學起來（最多 8 個 ID）')).toBeInTheDocument()
  expect(screen.getByText('還沒有學習任何 QR Code')).toBeInTheDocument()
  empty.unmount()
  const device = withLearned(new DemoDevice(() => 0), 6, [
    { id: 1, name: '', text: 'MXCam' },
    { id: 2, name: '左轉', text: 'LEFT' },
  ])
  const setName = vi.spyOn(device, 'setName').mockResolvedValue()
  const forget = vi.spyOn(device, 'forget').mockResolvedValue()
  setup(6, [], device)
  tab('學習')
  expect(screen.getByText('MXCam')).toBeInTheDocument()
  expect(screen.getByText('LEFT')).toBeInTheDocument()
  expect(screen.getByRole('textbox', { name: 'ID 2 名稱' })).toHaveValue('左轉')
  const input = screen.getByRole('textbox', { name: 'ID 1 名稱' })
  fireEvent.change(input, { target: { value: '起點' } })
  fireEvent.blur(input)
  await waitFor(() => expect(setName).toHaveBeenCalledWith(6, 1, '起點'))
  fireEvent.click(screen.getByRole('button', { name: '刪除 ID 2' }))
  await waitFor(() => expect(forget).toHaveBeenCalledWith(6, 2))
  expect(screen.getByRole('button', { name: '全部忘記' })).toBeEnabled()
})

test('QR learn tab: forget all, after the confirmation, forgets every QR code', async () => {
  const device = withLearned(new DemoDevice(() => 0), 6, [{ id: 1, name: '', text: 'MXCam' }])
  const forgetAll = vi.spyOn(device, 'forgetAll').mockResolvedValue()
  setup(6, [], device)
  tab('學習')
  fireEvent.click(screen.getByRole('button', { name: '全部忘記' }))
  fireEvent.click(screen.getByRole('button', { name: '確定忘記' }))
  await waitFor(() => expect(forgetAll).toHaveBeenCalledWith(6))
})

test('shape learn tab: no learning, four rows to name; naming sends setName(11, id, name)', async () => {
  const { device } = setup(11)
  const spy = vi.spyOn(device, 'setName')
  tab('學習')
  expect(screen.getByText(/幾何形狀不需要學習/)).toBeInTheDocument()
  expect(screen.getAllByRole('textbox')).toHaveLength(4)
  const input = screen.getByRole('textbox', { name: '三角形 名稱' })
  fireEvent.change(input, { target: { value: '屋頂' } })
  fireEvent.blur(input)
  await waitFor(() => expect(spy).toHaveBeenCalledWith(11, 2, '屋頂'))
  fireEvent.click(await screen.findByRole('button', { name: '刪除 三角形' }))
  await waitFor(() => expect(device.snapshot().learned[11]).toHaveLength(0))
})

test('motion learn tab says no learning is needed and has no controls', () => {
  setup(12)
  tab('學習')
  expect(screen.getByText('動態偵測不需要學習')).toBeInTheDocument()
  expect(screen.queryAllByRole('textbox')).toHaveLength(0)
  expect(screen.queryByRole('button', { name: '全部忘記' })).toBeNull()
})

test('QR results tab: ID, name, the whole content (with a note when cut short) and the box', () => {
  const long = 'x'.repeat(128)
  const device = withLearned(new DemoDevice(() => 0), 6, [{ id: 2, name: '左轉', text: 'LEFT' }])
  setup(
    6,
    [
      { id: 2, kind: 'qr', x: 100, y: 80, width: 41, height: 41, text: 'LEFT', corners: [[80, 60], [120, 60], [120, 100], [80, 100]] },
      { id: 0, kind: 'qr', x: 200, y: 80, width: 41, height: 41, text: long, truncated: true },
      { id: 0, kind: 'qr', x: 260, y: 80, width: 41, height: 41 },
    ],
    device,
  )
  expect(screen.getByRole('columnheader', { name: '內容' })).toBeInTheDocument()
  const rows = screen.getAllByRole('row').slice(1)
  expect(within(rows[0]).getAllByRole('cell').map((c) => c.textContent)).toEqual(['ID 2', '左轉', 'LEFT', '100', '80', '41', '41'])
  expect(within(rows[1]).getAllByRole('cell')[0]).toHaveTextContent('QR')
  expect(within(rows[1]).getAllByRole('cell')[2]).toHaveTextContent(long)
  expect(within(rows[1]).getAllByRole('cell')[2]).toHaveTextContent('內容太長，只顯示前 128 bytes')
  expect(within(rows[2]).getAllByRole('cell')[2]).toHaveTextContent('—')
})

test('shape results tab: shape name, confidence and the box', () => {
  setup(11, [{ id: 3, kind: 'shape', x: 80, y: 170, width: 54, height: 54, confidence: 90 }])
  expect(screen.getByRole('columnheader', { name: '形狀' })).toBeInTheDocument()
  const row = screen.getAllByRole('row')[1]
  expect(within(row).getAllByRole('cell').map((c) => c.textContent)).toEqual(['3', '正方形', '', '80', '170', '54', '54', '90%'])
})

test('motion results tab: the box and confidence, no name column', () => {
  setup(12, [{ id: 0, kind: 'motion', x: 160, y: 120, width: 60, height: 40, confidence: 63 }])
  expect(screen.queryByRole('columnheader', { name: '名稱' })).toBeNull()
  const row = screen.getAllByRole('row')[1]
  expect(within(row).getAllByRole('cell').map((c) => c.textContent)).toEqual(['動態', '160', '120', '60', '40', '63%'])
})
