import { useEffect, useRef, useState } from 'react'
import { useT } from '../../i18n'
import type { FunctionId } from '../layout/types'
import { snippetFor } from './codeSnippets'

export default function CodeTab({ fn }: { fn: FunctionId }) {
  const { t, lang } = useT()
  const [copied, setCopied] = useState<'ok' | 'fail' | null>(null)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current)
    },
    [],
  )
  const code = snippetFor(fn, lang)
  const show = (r: 'ok' | 'fail') => {
    setCopied(r)
    if (timer.current) clearTimeout(timer.current)
    timer.current = setTimeout(() => setCopied(null), 2500)
  }
  const copy = () => {
    const clip = typeof navigator !== 'undefined' ? navigator.clipboard : undefined
    if (!clip?.writeText) return show('fail')
    clip.writeText(code).then(
      () => show('ok'),
      () => show('fail'),
    )
  }
  return (
    <div className="rp-code">
      <div className="rp-code-bar">
        <p className="rp-hint">{t('codeNote')}</p>
        <button type="button" className="btn" onClick={copy}>
          {t('copy')}
        </button>
        <span role="status" className={copied === 'fail' ? 'rp-copied rp-copy-fail' : 'rp-copied'}>
          {copied === 'ok' ? t('copied') : copied === 'fail' ? t('copyFailed') : ''}
        </span>
      </div>
      <pre className="rp-pre" tabIndex={0} aria-label={t('tabCode')}>
        <code>{code}</code>
      </pre>
    </div>
  )
}
