// USB transport: Web Serial on the ESP32-S3's native USB-Serial-JTAG (the OTG port on the board).
import type { ByteTransport, TransportCandidate, TransportProvider } from './types'

export const MXCAM_USB_VID = 0x303a
export const MXCAM_USB_PID = 0x1001

function isMxcam(port: SerialPort): boolean {
  const i = port.getInfo()
  return i.usbVendorId === MXCAM_USB_VID && i.usbProductId === MXCAM_USB_PID
}

class WebSerialTransport implements ByteTransport {
  readonly kind = 'usb' as const
  readonly readable: ReadableStream<Uint8Array>
  readonly writable: WritableStream<Uint8Array>
  private readonly serial: Serial
  private readonly port: SerialPort
  private readonly lost: Array<() => void> = []
  private closed = false
  private readonly onDisconnect = (ev: Event) => {
    // Real events target the port; ignore ones for other ports.
    if (this.closed || ev.target !== this.port) return
    for (const cb of this.lost) cb()
  }

  constructor(serial: Serial, port: SerialPort, readable: ReadableStream<Uint8Array>, writable: WritableStream<Uint8Array>) {
    this.serial = serial
    this.port = port
    this.readable = readable
    this.writable = writable
    serial.addEventListener('disconnect', this.onDisconnect)
  }

  onLost(cb: () => void) {
    this.lost.push(cb)
  }

  async close(): Promise<void> {
    this.closed = true
    this.serial.removeEventListener('disconnect', this.onDisconnect)
    await this.port.close().catch(() => {})
  }
}

class WebSerialCandidate implements TransportCandidate {
  readonly kind = 'usb' as const
  private readonly serial: Serial
  private readonly port: SerialPort

  constructor(serial: Serial, port: SerialPort) {
    this.serial = serial
    this.port = port
  }

  async open(): Promise<ByteTransport> {
    const port = this.port
    try {
      await port.open({ baudRate: 115200 }) // baud is ignored by USB-Serial-JTAG but required by the API
      // The ESP32-S3 USB-Serial-JTAG treats DTR/RTS changes as reset/bootloader control, and
      // closing the port re-asserts them (RTS=1 with DTR=0 resets the chip). Drop RTS first,
      // then DTR, as two separate calls so the lines never pass through DTR=0/RTS=1.
      try {
        await port.setSignals({ requestToSend: false })
        await port.setSignals({ dataTerminalReady: false })
      } catch (e) {
        console.warn('setSignals failed, continuing', e)
      }
      if (!port.readable || !port.writable) throw new Error('port streams unavailable')
      return new WebSerialTransport(this.serial, port, port.readable, port.writable)
    } catch (e) {
      await port.close().catch(() => {})
      throw e
    }
  }
}

export class WebSerialProvider implements TransportProvider {
  readonly kind = 'usb' as const
  private readonly serial: Serial

  constructor(serial: Serial) {
    this.serial = serial
  }

  async request(): Promise<TransportCandidate | null> {
    try {
      const port = await this.serial.requestPort({ filters: [{ usbVendorId: MXCAM_USB_VID, usbProductId: MXCAM_USB_PID }] })
      return new WebSerialCandidate(this.serial, port)
    } catch (e) {
      if (e instanceof DOMException && e.name === 'NotFoundError') return null // user cancelled the picker
      throw e
    }
  }

  async granted(): Promise<TransportCandidate[]> {
    return (await this.serial.getPorts()).filter(isMxcam).map((p) => new WebSerialCandidate(this.serial, p))
  }

  onAttach(cb: (candidate: TransportCandidate | undefined) => void): () => void {
    const listener = (ev: Event) => {
      const t = ev.target as SerialPort | null
      cb(t && typeof t.getInfo === 'function' && isMxcam(t) ? new WebSerialCandidate(this.serial, t) : undefined)
    }
    this.serial.addEventListener('connect', listener)
    return () => this.serial.removeEventListener('connect', listener)
  }
}
