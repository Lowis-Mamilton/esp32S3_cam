import type { Result } from './types'

/** One GET_RESULT_DATA answer, already decoded: the content of record `index` of the batch with this frame id. */
export interface QrRead {
  frameId: number
  text: string
  truncated: boolean
}

/** A record whose centre is within this distance of a known code's last centre is that code (protocol units, px). */
const MATCH_PX = 32
/** With every record matched, the contents are read again at most this often (a card swapped in place). */
const REFRESH_MS = 1000

const decoder = new TextDecoder('utf-8', { fatal: false })
/** QR bytes to text: UTF-8 (byte mode is usually UTF-8, numeric and alphanumeric modes are ASCII); invalid bytes become U+FFFD. */
export const decodeQrText = (bytes: Uint8Array): string => decoder.decode(bytes)

interface Known {
  x: number
  y: number
  text: string
  truncated: boolean
}

/**
 * QR contents for RESULTS batches of function 6 (docs/protocol.md section 11). The content is not in the RESULTS
 * records: it is read with GET_RESULT_DATA, one request per record, and kept here. Later batches get it by position
 * (nearest centre within 32 px, the position then follows the record), so reads stay rare: only when a record has no
 * known content, or once a second, and never two reads at a time. A read whose answer belongs to another frame is
 * dropped as a whole (those records are not these boxes) and tried again with a later batch.
 */
export class QrTextCache {
  private readonly fetch: (index: number) => Promise<QrRead>
  private known: Known[] = []
  private inFlight = false
  private lastReadMs = -Infinity
  /** Bumped by reset(), so a read started before it cannot store its answer. */
  private gen = 0

  constructor(fetch: (index: number) => Promise<QrRead>) {
    this.fetch = fetch
  }

  /** Attaches the known contents to `results`; starts a read of this batch when needed (see the class comment). */
  annotate(results: Result[], frameId: number, nowMs: number): Result[] {
    const hit = this.match(results)
    const out = results.map((r, i) => {
      const k = hit[i]
      return k ? { ...r, text: k.text, truncated: k.truncated } : r
    })
    const unmatched = hit.some((k) => !k)
    if (results.length > 0 && !this.inFlight && (unmatched || nowMs - this.lastReadMs >= REFRESH_MS)) {
      void this.read(results, frameId, nowMs)
    }
    return out
  }

  reset(): void {
    this.gen++
    this.known = []
    this.inFlight = false
    this.lastReadMs = -Infinity
  }

  /** Pairs records with known codes, nearest first, each code at most once; matched codes move to the record. */
  private match(results: Result[]): (Known | undefined)[] {
    const pairs: { i: number; j: number; d: number }[] = []
    results.forEach((r, i) =>
      this.known.forEach((k, j) => {
        const d = Math.hypot(r.x - k.x, r.y - k.y)
        if (d <= MATCH_PX) pairs.push({ i, j, d })
      }),
    )
    pairs.sort((a, b) => a.d - b.d)
    const hit: (Known | undefined)[] = results.map(() => undefined)
    const used = new Set<number>()
    for (const { i, j } of pairs) {
      if (hit[i] || used.has(j)) continue
      used.add(j)
      hit[i] = this.known[j]
    }
    this.known = this.known.map((k, j) => {
      const i = hit.indexOf(k)
      return used.has(j) && i >= 0 ? { ...k, x: results[i].x, y: results[i].y } : k
    })
    return hit
  }

  private async read(results: Result[], frameId: number, nowMs: number) {
    const gen = this.gen
    this.inFlight = true
    this.lastReadMs = nowMs
    try {
      const got: Known[] = []
      for (let i = 0; i < results.length; i++) {
        const r = await this.fetch(i)
        if (gen !== this.gen || r.frameId !== frameId) return
        got.push({ x: results[i].x, y: results[i].y, text: r.text, truncated: r.truncated })
      }
      this.known = got
    } catch {
      // Lost reply, BAD_PARAM (the device has a newer batch with fewer records) or disconnected: a later batch reads again.
    } finally {
      if (gen === this.gen) this.inFlight = false
    }
  }
}
