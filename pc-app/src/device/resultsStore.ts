import { Emitter, type Result } from './types'

const RING = 32 // ~3 s of results at 10 fps
const EMPTY: Result[] = []

/**
 * Recognition results outside React state, so only the views that draw them re-render at push rate.
 * latest(): the newest batch (what the R4 would read). paired(): the newest batch whose frame id is not later
 * than the frame on screen, so boxes are drawn on the frame they were computed from (review point 4).
 */
export class ResultsStore {
  private readonly changes = new Emitter<[]>()
  private ring: { frameId: number | undefined; results: Result[] }[] = []
  private displayed: number | undefined
  private latestView: Result[] = EMPTY
  private pairedView: Result[] = EMPTY

  readonly subscribe = (cb: () => void): (() => void) => this.changes.on(cb)
  readonly latest = (): Result[] => this.latestView
  readonly paired = (): Result[] => this.pairedView

  push(results: Result[], frameId?: number) {
    this.ring.push({ frameId, results })
    if (this.ring.length > RING) this.ring.shift()
    this.update()
  }

  /** The frame now on screen; undefined when frames carry no id (demo). */
  setDisplayed(frameId: number | undefined) {
    if (this.displayed === frameId) return
    this.displayed = frameId
    this.update()
  }

  clear() {
    this.ring = []
    this.displayed = undefined
    this.update()
  }

  private select(): Result[] {
    const last = this.ring.at(-1)
    if (!last) return EMPTY
    if (this.displayed === undefined || last.frameId === undefined) return last.results
    for (let i = this.ring.length - 1; i >= 0; i--) {
      const b = this.ring[i]
      if (b.frameId !== undefined && b.frameId <= this.displayed) return b.results
    }
    return EMPTY
  }

  private update() {
    const latest = this.ring.at(-1)?.results ?? EMPTY
    const paired = this.select()
    if (latest === this.latestView && paired === this.pairedView) return
    this.latestView = latest
    this.pairedView = paired
    this.changes.emit()
  }
}
