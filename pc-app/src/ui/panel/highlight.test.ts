import { expect, test } from 'vitest'
import { snippetFor } from './codeSnippets'
import { tokenizeLine } from './highlight'

test('tokens rebuild every snippet line exactly', () => {
  for (const fn of [1, 2, 3] as const) {
    for (const lang of ['zh-TW', 'en'] as const) {
      for (const line of snippetFor(fn, lang).split('\n')) {
        expect(tokenizeLine(line).map((t) => t.text).join('')).toBe(line)
      }
    }
  }
})

test('comments win over what is inside them', () => {
  expect(tokenizeLine('  delay(50); // void "x" 12')).toEqual([
    { kind: 'text', text: '  delay(' },
    { kind: 'num', text: '50' },
    { kind: 'text', text: '); ' },
    { kind: 'com', text: '// void "x" 12' },
  ])
  expect(tokenizeLine('#include <MXCam.h>')[0]).toEqual({ kind: 'pre', text: '#include' })
  expect(tokenizeLine('cam.setAlgorithm(MXCAM_ALGO_FACE);').map((t) => t.kind)).toContain('const')
})
