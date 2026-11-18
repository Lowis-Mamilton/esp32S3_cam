import { render, screen } from '@testing-library/react'
import { Decoder, encodeFrame } from './protocol/codec'
import { Cmd, responseOf } from './protocol/messages'
import { expect, test } from 'vitest'
import App from './App'

const idleSerial = {
  addEventListener: () => {},
  removeEventListener: () => {},
  getPorts: async () => [],
  requestPort: async () => {
    throw new DOMException('cancel', 'NotFoundError')
  },
} as unknown as Serial

test('renders the MXCam Studio heading', () => {
  render(<App />)
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})

test('renders a single heading per test (cleanup between tests)', () => {
  render(<App />)
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})

test('without navigator.serial shows the unsupported message and no connect button', () => {
  render(<App />)
  expect(screen.getByText('此瀏覽器不支援，請用 Chrome 或 Edge')).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: '連線' })).not.toBeInTheDocument()
})

test('when disconnected shows the connect button and 未連線 status', () => {
  render(<App serial={idleSerial} />)
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
          ctl.enqueue(encodeFrame(responseOf(f.cmd), f.seq, f.cmd === Cmd.GetInfo ? new Uint8Array(23) : new Uint8Array(0)))
        }
      },
    }),
  }
  const serial = { ...idleSerial, getPorts: async () => [port] } as unknown as Serial
  render(<App serial={serial} />)
  expect(await screen.findByText('已連線')).toBeInTheDocument()
  ctl.close()
  expect(await screen.findByText('已斷線')).toBeInTheDocument()
})
