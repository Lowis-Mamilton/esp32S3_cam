// Bluetooth transport: Web Bluetooth (BLE GATT). The firmware must be a Bluetooth build.
// Requests are written to RX, replies and pushes arrive as notifications on TX; both are plain byte streams
// of MX packets, cut wherever the link likes.
import type { ByteTransport, TransportCandidate, TransportProvider } from './types'

export const MXCAM_BLE_NAME_PREFIX = 'MXCam-'
export const MXCAM_BLE_SERVICE = '4d584300-0010-4b1e-9c5a-7f3e2a1d0c9b'
/** Host to device (write). */
export const MXCAM_BLE_RX = '4d584300-0011-4b1e-9c5a-7f3e2a1d0c9b'
/** Device to host (notify). */
export const MXCAM_BLE_TX = '4d584300-0012-4b1e-9c5a-7f3e2a1d0c9b'
/** Largest single write; fits the smallest ATT MTU desktop browsers negotiate. */
export const BLE_WRITE_CHUNK = 180

// The parts of Web Bluetooth used here. TypeScript's DOM library has no Web Bluetooth types, and tests
// pass a fake with the same shape.
export interface BluetoothCharacteristicLike {
  value?: DataView | null
  startNotifications(): Promise<unknown>
  writeValueWithResponse(data: Uint8Array): Promise<void>
  addEventListener(type: 'characteristicvaluechanged', listener: (ev: Event) => void): void
  removeEventListener(type: 'characteristicvaluechanged', listener: (ev: Event) => void): void
}
export interface BluetoothServiceLike {
  getCharacteristic(uuid: string): Promise<BluetoothCharacteristicLike>
}
export interface BluetoothGattLike {
  connect(): Promise<{ getPrimaryService(uuid: string): Promise<BluetoothServiceLike> }>
  disconnect(): void
}
export interface BluetoothDeviceLike {
  name?: string
  gatt?: BluetoothGattLike
  addEventListener(type: 'gattserverdisconnected', listener: () => void): void
  removeEventListener(type: 'gattserverdisconnected', listener: () => void): void
}
export interface BluetoothLike {
  requestDevice(options: { filters: { namePrefix: string }[]; optionalServices: string[] }): Promise<BluetoothDeviceLike>
  getAvailability?(): Promise<boolean>
}

/** navigator.bluetooth when this browser has Web Bluetooth. */
export function browserBluetooth(): BluetoothLike | undefined {
  if (typeof navigator === 'undefined') return undefined
  return (navigator as Navigator & { bluetooth?: BluetoothLike }).bluetooth
}

/**
 * A Bluetooth failure with a known meaning:
 * - unavailable: the computer's Bluetooth is off or missing
 * - connect-failed: the board was picked but the link could not be set up
 * - no-service: the board answers but has no MXCam Bluetooth service (firmware without Bluetooth)
 */
export type BluetoothErrorCode = 'unavailable' | 'connect-failed' | 'no-service'
export class BluetoothError extends Error {
  readonly code: BluetoothErrorCode
  constructor(code: BluetoothErrorCode, message: string) {
    super(message)
    this.name = 'BluetoothError'
    this.code = code
  }
}

// Read by shape, not instanceof Error: a DOMException is not an Error in every environment.
const field = (e: unknown, key: 'name' | 'message'): string | undefined =>
  typeof e === 'object' && e !== null && key in e ? String((e as Record<string, unknown>)[key]) : undefined
export const errName = (e: unknown) => field(e, 'name') ?? ''
export const errText = (e: unknown) => field(e, 'message') ?? String(e)

class WebBluetoothTransport implements ByteTransport {
  readonly kind = 'bluetooth' as const
  readonly readable: ReadableStream<Uint8Array>
  readonly writable: WritableStream<Uint8Array>
  private readonly device: BluetoothDeviceLike
  private readonly tx: BluetoothCharacteristicLike
  private ctl!: ReadableStreamDefaultController<Uint8Array>
  private readonly lost: Array<() => void> = []
  private closed = false
  private readonly onValue = (ev: Event) => {
    const v = (ev.target as BluetoothCharacteristicLike | null)?.value
    if (!v || this.closed) return
    try {
      // Copy: the browser may reuse the buffer for the next notification.
      this.ctl.enqueue(new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)))
    } catch {
      // the reader was cancelled (teardown in progress)
    }
  }
  private readonly onDisconnected = () => {
    if (this.closed) return
    this.endReadable()
    for (const cb of this.lost) cb()
  }

  constructor(device: BluetoothDeviceLike, rx: BluetoothCharacteristicLike, tx: BluetoothCharacteristicLike) {
    this.device = device
    this.tx = tx
    this.readable = new ReadableStream<Uint8Array>({
      start: (c) => {
        this.ctl = c
      },
    })
    // The stream runs one write() at a time, so chunks reach the board in order and never overlap
    // (Web Bluetooth rejects a GATT operation started while another is in progress).
    this.writable = new WritableStream<Uint8Array>({
      write: async (bytes) => {
        for (let off = 0; off < bytes.length; off += BLE_WRITE_CHUNK) {
          await rx.writeValueWithResponse(bytes.slice(off, off + BLE_WRITE_CHUNK))
        }
      },
    })
    tx.addEventListener('characteristicvaluechanged', this.onValue)
    device.addEventListener('gattserverdisconnected', this.onDisconnected)
  }

  /** Notifications must be on before the first request is written. */
  start(): Promise<unknown> {
    return this.tx.startNotifications()
  }

  onLost(cb: () => void) {
    this.lost.push(cb)
  }

  private endReadable() {
    try {
      this.ctl.close()
    } catch {
      // already closed or cancelled
    }
  }

  async close(): Promise<void> {
    if (this.closed) return
    this.closed = true
    this.tx.removeEventListener('characteristicvaluechanged', this.onValue)
    this.device.removeEventListener('gattserverdisconnected', this.onDisconnected)
    this.endReadable()
    try {
      this.device.gatt?.disconnect()
    } catch {
      // already disconnected
    }
  }
}

class WebBluetoothCandidate implements TransportCandidate {
  readonly kind = 'bluetooth' as const
  private readonly device: BluetoothDeviceLike

  constructor(device: BluetoothDeviceLike) {
    this.device = device
  }

  async open(): Promise<ByteTransport> {
    const gatt = this.device.gatt
    if (!gatt) throw new BluetoothError('connect-failed', 'device has no GATT server')
    let transport: WebBluetoothTransport | null = null
    try {
      let server: Awaited<ReturnType<BluetoothGattLike['connect']>>
      try {
        server = await gatt.connect()
      } catch (e) {
        throw new BluetoothError('connect-failed', errText(e))
      }
      let rx: BluetoothCharacteristicLike
      let tx: BluetoothCharacteristicLike
      try {
        const service = await server.getPrimaryService(MXCAM_BLE_SERVICE)
        rx = await service.getCharacteristic(MXCAM_BLE_RX)
        tx = await service.getCharacteristic(MXCAM_BLE_TX)
      } catch (e) {
        // NotFoundError: connected, but the board does not offer the service.
        throw new BluetoothError(errName(e) === 'NotFoundError' ? 'no-service' : 'connect-failed', errText(e))
      }
      transport = new WebBluetoothTransport(this.device, rx, tx)
      try {
        await transport.start()
      } catch (e) {
        throw new BluetoothError('connect-failed', errText(e))
      }
      return transport
    } catch (e) {
      if (transport) await transport.close()
      else {
        try {
          gatt.disconnect()
        } catch {
          // not connected
        }
      }
      throw e
    }
  }
}

export class WebBluetoothProvider implements TransportProvider {
  readonly kind = 'bluetooth' as const
  private readonly bluetooth: BluetoothLike

  constructor(bluetooth: BluetoothLike) {
    this.bluetooth = bluetooth
  }

  async request(): Promise<TransportCandidate | null> {
    try {
      // First thing in the click handler's call chain: the device chooser needs the user gesture.
      const device = await this.bluetooth.requestDevice({
        filters: [{ namePrefix: MXCAM_BLE_NAME_PREFIX }],
        optionalServices: [MXCAM_BLE_SERVICE],
      })
      return new WebBluetoothCandidate(device)
    } catch (e) {
      if (errName(e) !== 'NotFoundError') throw e
      // NotFoundError is both "the user closed the chooser" and "there is no usable adapter".
      if (/adapter/i.test(errText(e))) throw new BluetoothError('unavailable', errText(e))
      const available = this.bluetooth.getAvailability ? await this.bluetooth.getAvailability().catch(() => true) : true
      if (!available) throw new BluetoothError('unavailable', 'Bluetooth is not available')
      return null
    }
  }

  /** Bluetooth always needs a click: there is nothing to reconnect to on its own. */
  async granted(): Promise<TransportCandidate[]> {
    return []
  }

  onAttach(): () => void {
    return () => {}
  }
}
