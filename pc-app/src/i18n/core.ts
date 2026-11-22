import type { TransportKind } from '../transport/types'
import { BluetoothError, errName, errText } from '../transport/webBluetooth'
import { en } from './en'
import { zhTW } from './zh-TW'

export type Lang = 'zh-TW' | 'en'
export type MessageKey = keyof typeof zhTW

export const dicts: Record<Lang, Record<MessageKey, string>> = { 'zh-TW': zhTW, en }

/** Pure lookup, exported for tests. `{name}` placeholders are replaced from vars. */
export function translate(lang: Lang, key: MessageKey, vars?: Record<string, string | number>): string {
  const text = dicts[lang][key]
  if (!vars) return text
  return text.replace(/\{(\w+)\}/g, (m, k: string) => (k in vars ? String(vars[k]) : m))
}

export interface ErrorText {
  key: MessageKey
  /** Raw error text, shown small, only for unmapped errors. */
  detail?: string
}

const BLUETOOTH_ERROR: Record<BluetoothError['code'], MessageKey> = {
  unavailable: 'errBtUnavailable',
  'connect-failed': 'errBtConnect',
  'no-service': 'errBtFirmware',
}

/**
 * Map a connect/open failure to a message key (resolved with t() at render time).
 * `kind` is the transport the user asked for; the serial-port messages only apply to USB.
 */
export function describeError(e: unknown, kind?: TransportKind): ErrorText {
  if (e instanceof BluetoothError) return { key: BLUETOOTH_ERROR[e.code] }
  const name = errName(e)
  const msg = errText(e)
  if (kind === 'bluetooth') {
    if (/timeout|not responding/i.test(msg)) return { key: 'errNoResponse' }
    // Browser GATT failures (link dropped while setting up, operation failed).
    if (name === 'NetworkError' || name === 'InvalidStateError' || /gatt/i.test(msg)) return { key: 'errBtConnect' }
    return { key: 'errGeneric', detail: msg }
  }
  if (name === 'NetworkError' || /failed to open/i.test(msg)) return { key: 'errPortBusy' }
  if (name === 'InvalidStateError') return { key: 'errAlreadyOpen' }
  if (/timeout|not responding/i.test(msg)) return { key: 'errNoResponse' }
  return { key: 'errGeneric', detail: msg }
}
