// @vitest-environment node
// The few facts of src/theme.css that the phone / tablet layout rests on (docs/adr/0023). jsdom does not lay out
// CSS, so the stylesheet is read as text here; the matching DOM structure is checked in src/responsive.test.tsx.
import { readFileSync } from 'node:fs'
import { expect, test } from 'vitest'

// Line endings differ between checkouts (CRLF on Windows); the patterns below only know "\n".
const css = readFileSync(new URL('./src/theme.css', import.meta.url), 'utf8').replaceAll('\r\n', '\n')

/** The body of the first `@media` block whose query contains `query` (braces are balanced in theme.css). */
function mediaBlock(query: string): string {
  const at = css.indexOf(`@media ${query}`)
  if (at < 0) return ''
  const open = css.indexOf('{', at)
  let depth = 0
  for (let i = open; i < css.length; i++) {
    if (css[i] === '{') depth++
    else if (css[i] === '}' && --depth === 0) return css.slice(open + 1, i)
  }
  return ''
}

test('theme.css: no fixed minimum page width; narrow is below 960 px and chosen by a media query', () => {
  expect(css).not.toMatch(/min-width:\s*1024px/)
  const narrow = mediaBlock('(max-width: 959.98px) {')
  expect(narrow).not.toBe('')
  // The class names the narrow layout rearranges; renaming one in the markup must be followed here.
  for (const name of ['topbar', 'tb-start', 'tb-centre', 'tb-end', 'tb-seg', 'tb-connect', 'app-columns', 'sidebar', 'nav-item', 'nav-rule', 'app-center', 'stage-wrap', 'ls-hat', 'ls-pin', 'stage', 'stage-actions', 'rightpanel', 'rp-tabs', 'rp-tabpanel', 'rp-savebar', 'pg', 'pg-row', 'pg-tiles']) {
    expect(narrow, name).toMatch(new RegExp(`\\.${name}[\\s,{]`))
  }
  // One column: the boxes of the old columns dissolve, the save bar sticks to the bottom.
  expect(narrow).toMatch(/\.app-center,\s*\.stage-wrap,\s*\.rightpanel\s*\{\s*display:\s*contents/)
  expect(narrow).toMatch(/\.rp-savebar\s*\{[^}]*position:\s*sticky;[^}]*bottom:\s*0/)
  expect(narrow).toMatch(/\.sidebar\s*\{[^}]*overflow-x:\s*auto/)
})

test('theme.css: the picture is pinned only when the screen is tall enough', () => {
  const narrow = mediaBlock('(max-width: 959.98px) {')
  expect(narrow).not.toMatch(/\.ls-pin\s*\{[^}]*position:\s*sticky/)
  const tall = mediaBlock('(max-width: 959.98px) and (min-height: 600px)')
  expect(tall).toMatch(/\.ls-pin\s*\{[^}]*position:\s*sticky;[^}]*top:\s*0/)
})

test('theme.css: touch-action none only on a learnable stage; 44 px targets for coarse pointers', () => {
  expect(css).toMatch(/\.stage\.learnable\s*\{[^}]*touch-action:\s*none/)
  expect(css.match(/touch-action:\s*none/g)).toHaveLength(1)
  const coarse = mediaBlock('(pointer: coarse)')
  for (const name of ['btn', 'nav-item', 'tb-seg button', 'rp-tabs button', 'tb-switch', 'ls-mirror', 'rp-icon', 'rp-input']) {
    expect(coarse, name).toContain(`.${name}`)
  }
  expect(coarse).toMatch(/min-height:\s*44px/)
  expect(coarse).toMatch(/::-webkit-slider-thumb\s*\{[^}]*width:\s*44px;[^}]*height:\s*44px/)
})
