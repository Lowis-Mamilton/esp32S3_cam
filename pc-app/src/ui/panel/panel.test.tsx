import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { useSyncExternalStore } from 'react'
import { afterEach, beforeEach, expect, test, vi } from 'vitest'
import { DemoDevice } from '../../device/DemoDevice'
import { RealDevice } from '../../device/RealDevice'
import type { Device, FunctionId, Result } from '../../device/types'
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
afterEach(() => {
  devices.forEach((d) => d.dispose())
  devices = []
  vi.restoreAllMocks()
})

function Harness({ device, fn, results }: { device: Device; fn: FunctionId; results: Result[] }) {
  const snapshot = useSyncExternalStore(
    (cb) => device.subscribe(cb),
    () => device.snapshot(),
  )
  return <RightPanel device={device} snapshot={snapshot} fn={fn} results={results} />
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
    await device.learnColor(FULL)
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
    await device.learnColor(FULL)
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

test('face results show a confidence column', () => {
  setup(3, [{ id: 0, kind: 'face', x: 1, y: 2, width: 3, height: 4, confidence: 87.6 }])
  expect(screen.getByRole('columnheader', { name: '信心度' })).toBeInTheDocument()
  expect(screen.getByText('88%')).toBeInTheDocument()
})

test('renaming a learned colour shows the new name in the results tab', async () => {
  const { device } = setup(1, [{ id: 1, kind: 'color', x: 1, y: 2, width: 3, height: 4 }])
  await act(async () => {
    await device.learnColor(FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  fireEvent.change(input, { target: { value: '紅積木' } })
  fireEvent.blur(input)
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('紅積木'))
  tab('結果')
  expect(screen.getByRole('row', { name: /ID1/ })).toHaveTextContent('紅積木')
})

test('name input is limited to 32 characters and Enter commits', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learnColor(FULL)
  })
  tab('學習')
  const input = screen.getByRole('textbox', { name: 'ID1 名稱' })
  expect(input).toHaveAttribute('maxlength', '32')
  fireEvent.change(input, { target: { value: 'abc' } })
  fireEvent.keyDown(input, { key: 'Enter' })
  await waitFor(() => expect(device.snapshot().learned[1][0].name).toBe('abc'))
})

test('delete forgets a single ID', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learnColor(FULL)
  })
  tab('學習')
  fireEvent.click(screen.getByRole('button', { name: '刪除 ID1' }))
  await waitFor(() => expect(device.snapshot().learned[1]).toHaveLength(0))
})

test('全部忘記 needs an inline confirmation; cancel does nothing', async () => {
  const { device } = setup()
  await act(async () => {
    await device.learnColor(FULL)
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

test('real device without the function: controls are disabled with a firmware hint, nothing throws', () => {
  const real = new RealDevice(undefined)
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
