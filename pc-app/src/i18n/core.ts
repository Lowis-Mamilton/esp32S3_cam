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

/** Map a connect/open failure to a message key (resolved with t() at render time). */
export function describeError(e: unknown): ErrorText {
  const name = e instanceof Error ? e.name : ''
  const msg = e instanceof Error ? e.message : String(e)
  if (name === 'NetworkError' || /failed to open/i.test(msg)) return { key: 'errPortBusy' }
  if (name === 'InvalidStateError') return { key: 'errAlreadyOpen' }
  if (/timeout|not responding/i.test(msg)) return { key: 'errNoResponse' }
  return { key: 'errGeneric', detail: msg }
}
