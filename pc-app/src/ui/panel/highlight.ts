/** Token kinds for the light Arduino colouring of the code tab. */
export type TokenKind = 'com' | 'str' | 'pre' | 'kw' | 'type' | 'const' | 'num' | 'text'
export interface Token {
  kind: TokenKind
  text: string
}

// Order matters: comments and strings first so nothing inside them is coloured.
const RULES: [TokenKind, RegExp][] = [
  ['com', /\/\/.*/],
  ['str', /"(?:[^"\\]|\\.)*"/],
  ['pre', /#\w+/],
  ['const', /\bMXCAM_\w+\b/],
  ['kw', /\b(?:void|if|else|while|for|return|true|false)\b/],
  ['type', /\b(?:MXCam|MXCamResult|Serial1?|int|bool)\b/],
  ['num', /\b\d+\b/],
]
const RE = new RegExp(RULES.map(([, re]) => `(${re.source})`).join('|'), 'g')

/** Splits one line into tokens; joining the token texts gives the line back unchanged. */
export function tokenizeLine(line: string): Token[] {
  const out: Token[] = []
  let last = 0
  for (const m of line.matchAll(RE)) {
    const at = m.index
    if (at > last) out.push({ kind: 'text', text: line.slice(last, at) })
    const group = m.slice(1).findIndex((g) => g !== undefined)
    out.push({ kind: RULES[group][0], text: m[0] })
    last = at + m[0].length
  }
  if (last < line.length) out.push({ kind: 'text', text: line.slice(last) })
  return out
}
