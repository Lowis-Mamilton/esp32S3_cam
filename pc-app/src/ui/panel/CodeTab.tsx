import { useEffect, useRef, useState } from 'react'
import { useT } from '../../i18n'
import type { FunctionId } from '../layout/types'
import { snippetFor } from './codeSnippets'

export default function CodeTab({ fn }: { fn: FunctionId }) {
  const { t, lang } = useT()
  const [copied, setCopied] = useState(false)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current)
    },
    [],
  )
  const code = snippetFor(fn, lang)
  const copy = () => {
    const clip = typeof navigator !== 'undefined' ? navigator.clipboard : undefined
    if (!clip?.writeText) return
    clip
      .writeText(code)
      .then(() => {
        setCopied(true)
        if (timer.current) clearTimeout(timer.current)
        timer.current = setTimeout(() => setCopied(false), 1800)
      })
      .catch(() => {})
  }
  return (
    <div className="rp-code">
      <div className="rp-code-bar">
        <p className="rp-hint">{t('codeNote')}</p>
        <button type="button" className="btn" onClick={copy}>
          {t('copy')}
        </button>
        <span role="status" className="rp-copied">
          {copied ? t('copied') : ''}
        </span>
      </div>
      <pre className="rp-pre" tabIndex={0} aria-label={t('tabCode')}>
        <code>{code}</code>
      </pre>
    </div>
  )
}
