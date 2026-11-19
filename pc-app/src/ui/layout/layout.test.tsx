import { fireEvent, render, screen } from '@testing-library/react'
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
  configKnown: true,
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
