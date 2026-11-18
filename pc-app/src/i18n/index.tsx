import { createContext, useCallback, useEffect, useContext, useMemo, useState, type ReactNode } from 'react'
import { translate, type Lang, type MessageKey } from './core'

export type { Lang, MessageKey }

const STORAGE_KEY = 'mxcam.lang'

function readLang(): Lang {
  try {
    const v = localStorage.getItem(STORAGE_KEY)
    if (v === 'zh-TW' || v === 'en') return v
  } catch {
    // storage unavailable
  }
  return 'zh-TW'
}

interface I18n {
  t: (key: MessageKey, vars?: Record<string, string | number>) => string
  lang: Lang
  setLang(l: Lang): void
}

const Ctx = createContext<I18n | null>(null)

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(readLang)
  useEffect(() => {
    document.documentElement.lang = lang
  }, [lang])
  const setLang = useCallback((l: Lang) => {
    setLangState(l)
    try {
      localStorage.setItem(STORAGE_KEY, l)
    } catch {
      // storage unavailable
    }
  }, [])
  const value = useMemo<I18n>(
    () => ({ t: (key, vars) => translate(lang, key, vars), lang, setLang }),
    [lang, setLang],
  )
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}

// oxlint-disable-next-line react/only-export-components -- hook colocated with its provider
export function useT(): I18n {
  const v = useContext(Ctx)
  if (!v) throw new Error('useT must be used inside I18nProvider')
  return v
}

export type TFn = ReturnType<typeof useT>['t']
