import { afterEach, describe, expect, test, vi } from 'vitest'
import { RealDevice } from '../device/RealDevice'
import { describeError } from '../i18n/core'
import { Cmd } from '../protocol/messages'
import { MxcamConnection, type ConnectionStatus } from '../serial/connection'
import { FakeFirmware } from '../testing/fakeFirmware'
import { BLE_WRITE_CHUNK, BluetoothError, MXCAM_BLE_RX, MXCAM_BLE_SERVICE, MXCAM_BLE_TX } from './webBluetooth'

function setup(serial?: Serial) {
  const fw = new FakeFirmware()
  const statuses: ConnectionStatus[] = []
  const frames: number[] = []
  const infos: number[][] = []
  const conn = new MxcamConnection(
    serial,
    {
      status: (s) => statuses.push(s),
      frame: (f) => frames.push(f.frameId),
      stats: () => {},
      info: (i) => infos.push(i.functionIds),
    },
    { bluetooth: fw.bluetooth() },
  )
  return { fw, conn, statuses, frames, infos }
}

const streamStarts = (fw: FakeFirmware) => fw.requests.filter((r) => r.cmd === Cmd.StreamStart).map((r) => [...r.payload])

afterEach(() => {
  vi.useRealTimers()
})

describe('Bluetooth transport', () => {
  test('the UUIDs and write size are the ones agreed with the firmware', () => {
    expect(MXCAM_BLE_SERVICE).toBe('4d584300-0010-4b1e-9c5a-7f3e2a1d0c9b')
    expect(MXCAM_BLE_RX).toBe('4d584300-0011-4b1e-9c5a-7f3e2a1d0c9b')
    expect(MXCAM_BLE_TX).toBe('4d584300-0012-4b1e-9c5a-7f3e2a1d0c9b')
    expect(BLE_WRITE_CHUNK).toBe(180)
  })

  test('connect over Bluetooth: picker filter, GET_INFO, then connected', async () => {
    const { fw, conn, statuses, infos } = setup()
    expect(conn.kinds).toEqual(['bluetooth'])
    await conn.connect('bluetooth')
    expect(fw.btRequestOptions).toEqual({ filters: [{ namePrefix: 'MXCam-' }], optionalServices: [MXCAM_BLE_SERVICE] })
    expect(fw.cmds()).toEqual([Cmd.GetInfo, Cmd.StreamStart])
    expect(infos).toEqual([[1, 2]])
    expect(statuses).toEqual(['connecting', 'connected'])
    expect(conn.transport).toBe('bluetooth')
    await conn.disconnect()
    expect(statuses.at(-1)).toBe('disconnected')
    expect(conn.transport).toBeNull()
    expect(fw.btConnected).toBe(false)
  })

  test('without a kind, connect uses Bluetooth when it is the only transport', async () => {
    const { conn, statuses } = setup()
    await conn.connect()
    expect(statuses.at(-1)).toBe('connected')
    expect(conn.transport).toBe('bluetooth')
    await conn.disconnect()
  })

  test('a FRAME push split across several notifications is decoded', async () => {
    const { fw, conn, frames } = setup()
    fw.btNotifyChunk = 7 // the 25-byte packet arrives in 4 notifications
    await conn.connect('bluetooth')
    fw.pushFrame(9)
    fw.pushFrame(10)
    await vi.waitFor(() => expect(frames).toEqual([9, 10]))
    await conn.disconnect()
  })

  test('requests are cut into writes of at most 180 bytes, one at a time and in order', async () => {
    const { fw, conn } = setup()
    await conn.connect('bluetooth')
    fw.btWrites.length = 0
    const big = new Uint8Array(400) // 409 bytes on the wire: 180 + 180 + 49
    const results = await Promise.allSettled([conn.request(Cmd.SetName, big), conn.request(Cmd.Ping), conn.request(Cmd.GetFunction)])
    expect(results.map((r) => r.status)).toEqual(['rejected', 'fulfilled', 'fulfilled']) // SET_NAME of zeros is BAD_PARAM
    expect(fw.btWrites).toEqual([180, 180, 49, 9, 9])
    expect(fw.btMaxConcurrentWrites).toBe(1)
    const last = fw.requests.slice(-3)
    expect(last.map((r) => r.cmd)).toEqual([Cmd.SetName, Cmd.Ping, Cmd.GetFunction])
    expect(last[0].payload.length).toBe(400)
    await conn.disconnect()
  })

  test('STREAM_START asks for 5 fps on Bluetooth and 10 on USB', async () => {
    const bt = setup()
    await bt.conn.connect('bluetooth')
    expect(streamStarts(bt.fw)).toEqual([[1, 5, 0]])
    await bt.conn.disconnect()

    const usbFw = new FakeFirmware()
    const usb = new MxcamConnection(usbFw.serial(), { status: () => {}, frame: () => {}, stats: () => {} }, { bluetooth: usbFw.bluetooth() })
    expect(usb.kinds).toEqual(['usb', 'bluetooth'])
    await usb.connect('usb')
    expect(usb.transport).toBe('usb')
    expect(streamStarts(usbFw)).toEqual([[1, 10, 0]])
    await usb.disconnect()
  })

  test('the watchdog re-send keeps 5 fps on Bluetooth', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval', 'setTimeout', 'clearTimeout'] })
    const { fw, conn } = setup()
    await conn.connect('bluetooth')
    await vi.advanceTimersByTimeAsync(3000)
    expect(streamStarts(fw)).toEqual([
      [1, 5, 0],
      [1, 5, 0],
    ])
    await conn.disconnect()
  })

  test('the link dropping tears the session down and rejects pending requests', async () => {
    const { fw, conn, statuses } = setup()
    await conn.connect('bluetooth')
    fw.drop.set(Cmd.GetFunction, 1)
    const pending = conn.request(Cmd.GetFunction, new Uint8Array(0), 5000).then(
      () => null,
      (e: unknown) => e,
    )
    await vi.waitFor(() => expect(fw.requests.at(-1)?.cmd).toBe(Cmd.GetFunction))
    fw.btDrop()
    expect(await pending).toBeInstanceOf(Error)
    expect(statuses.at(-1)).toBe('disconnected')
    await expect(conn.request(Cmd.Ping)).rejects.toThrow()
  })

  test('closing the device chooser is silent', async () => {
    const { fw, conn, statuses } = setup()
    fw.btPicker = 'cancel'
    await conn.connect('bluetooth')
    expect(statuses).toEqual([])
    expect(conn.transport).toBeNull()
  })

  test('no adapter: connect rejects with BluetoothError unavailable', async () => {
    const { fw, conn, statuses } = setup()
    fw.btPicker = 'no-adapter'
    await expect(conn.connect('bluetooth')).rejects.toMatchObject({ name: 'BluetoothError', code: 'unavailable' })
    expect(statuses).toEqual([])
  })

  test('a chooser closed while Bluetooth is off reports unavailable', async () => {
    const fw = new FakeFirmware()
    fw.btPicker = 'cancel'
    const conn = new MxcamConnection(undefined, { status: () => {}, frame: () => {}, stats: () => {} }, { bluetooth: { ...fw.bluetooth(), getAvailability: async () => false } })
    await expect(conn.connect('bluetooth')).rejects.toMatchObject({ code: 'unavailable' })
  })

  test('a board without the MXCam service: no-service, and the GATT link is released', async () => {
    const { fw, conn, statuses } = setup()
    fw.btHasService = false
    await expect(conn.connect('bluetooth')).rejects.toMatchObject({ name: 'BluetoothError', code: 'no-service' })
    expect(statuses).toEqual(['connecting', 'disconnected'])
    expect(fw.btConnected).toBe(false)
    expect(conn.transport).toBeNull()
  })

  test('Bluetooth never connects on its own', async () => {
    const { conn, statuses } = setup()
    expect(await conn.autoConnect()).toBe(false)
    expect(statuses).toEqual([])
  })
})

describe('RealDevice over Bluetooth', () => {
  test('Bluetooth alone is enough: not unsupported, and it is the only transport offered', () => {
    const fw = new FakeFirmware()
    const d = new RealDevice(undefined, undefined, fw.bluetooth())
    expect(d.snapshot().status).toBe('disconnected')
    expect(d.snapshot().transports).toEqual(['bluetooth'])
    expect(d.snapshot().transport).toBeNull()
    d.dispose()
  })

  test('snapshot says which transport is in use; an unexpected drop is lost and keeps it', async () => {
    const fw = new FakeFirmware()
    const d = new RealDevice(undefined, undefined, fw.bluetooth())
    await d.connect('bluetooth')
    expect(d.snapshot()).toMatchObject({ status: 'connected', transport: 'bluetooth', lost: false })
    fw.btDrop()
    await vi.waitFor(() => expect(d.snapshot().status).toBe('disconnected'))
    expect(d.snapshot()).toMatchObject({ lost: true, transport: 'bluetooth' })
    // Connecting again clears it.
    await d.connect('bluetooth')
    expect(d.snapshot()).toMatchObject({ status: 'connected', lost: false, transport: 'bluetooth' })
    await d.disconnect()
    expect(d.snapshot()).toMatchObject({ status: 'disconnected', lost: false, transport: null })
    d.dispose()
  })

  test('with both transports USB still auto-connects and reports usb', async () => {
    const fw = new FakeFirmware()
    const d = new RealDevice(fw.serial(), undefined, fw.bluetooth())
    expect(d.snapshot().transports).toEqual(['usb', 'bluetooth'])
    await vi.waitFor(() => expect(d.snapshot().status).toBe('connected'))
    expect(d.snapshot().transport).toBe('usb')
    expect(fw.btConnected).toBe(false)
    d.dispose()
  })
})

describe('describeError', () => {
  test('Bluetooth failures get their own messages', () => {
    expect(describeError(new BluetoothError('unavailable', 'x')).key).toBe('errBtUnavailable')
    expect(describeError(new BluetoothError('connect-failed', 'x')).key).toBe('errBtConnect')
    expect(describeError(new BluetoothError('no-service', 'x')).key).toBe('errBtFirmware')
    expect(describeError(new DOMException('GATT Server is disconnected.', 'NetworkError'), 'bluetooth').key).toBe('errBtConnect')
    expect(describeError(new Error('request 0x1 timeout'), 'bluetooth').key).toBe('errNoResponse')
  })

  test('serial-port messages only apply to USB', () => {
    const busy = new DOMException('Failed to open serial port.', 'NetworkError')
    expect(describeError(busy).key).toBe('errPortBusy')
    expect(describeError(busy, 'usb').key).toBe('errPortBusy')
    expect(describeError(new DOMException('x', 'InvalidStateError'), 'usb').key).toBe('errAlreadyOpen')
    expect(describeError(new DOMException('x', 'InvalidStateError'), 'bluetooth').key).toBe('errBtConnect')
  })
})
