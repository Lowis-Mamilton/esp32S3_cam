import { NAME_MAX_BYTES, emptyLearned, nameBytes, type FunctionId, type FunctionParams, type LearnedId, type SystemConfig } from './types'

/** Versioned device configuration, as exported/imported by Studio. */
export interface Config {
  learned: Record<FunctionId, LearnedId[]>
  /**
   * gesture, line, track, shape and motion are optional: files from before M8a / M6 / M7 have none, and absent
   * leaves the board alone.
   */
  params: Omit<FunctionParams, 'gesture' | 'line' | 'track' | 'shape' | 'motion'> & {
    gesture?: FunctionParams['gesture']
    line?: FunctionParams['line']
    track?: FunctionParams['track']
    shape?: FunctionParams['shape']
    motion?: FunctionParams['motion']
  }
  system: SystemConfig
}

const FNS: FunctionId[] = [1, 2, 3]
/** Optional lists: files from before M8a (8), M6 (4, 5) or M7 (6, 11, 12) have none. */
const OPTIONAL_FNS: FunctionId[] = [8, 4, 5, 6, 11, 12]
const MAX_GESTURE_ID = 9
const MAX_TAG_ID = 586 // tag36h11 has 587 codes
const MAX_QR_ID = 8
const MAX_SHAPE_ID = 4
/** QR content is at most 128 bytes, so its decoded text is at most 128 UTF-16 units. */
const MAX_QR_TEXT = 128

/** Valid learned IDs per function. */
function validId(fn: FunctionId, id: unknown): boolean {
  switch (fn) {
    case 2:
      return isInt(id, 0, MAX_TAG_ID)
    case 8:
      return isInt(id, 1, MAX_GESTURE_ID)
    case 4:
    case 5:
      return id === 1 // line colour and tracked object: id 1 only
    case 6:
      return isInt(id, 1, MAX_QR_ID)
    case 11:
      return isInt(id, 1, MAX_SHAPE_ID)
    case 12:
      return false // motion has nothing learned or named
    default:
      return isInt(id, 1, 7) // colour; face uses the same slots
  }
}

const isObj = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v)
const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const isInt = (v: unknown, min: number, max = Infinity): v is number => Number.isInteger(v) && (v as number) >= min && (v as number) <= max
const inRange = (v: unknown, min: number, max: number): v is number => isNum(v) && v >= min && v <= max
const isBool = (v: unknown): v is boolean => typeof v === 'boolean'

export const I2C_MIN = 0x08
export const I2C_MAX = 0x77
export const isValidI2cAddress = (v: unknown): v is number => isInt(v, I2C_MIN, I2C_MAX)

/** The file itself is malformed (as opposed to a device refusing it). */
export class ConfigFileError extends Error {
  constructor(what: string) {
    super(`invalid config: ${what}`)
    this.name = 'ConfigFileError'
  }
}

function bad(what: string): never {
  throw new ConfigFileError(what)
}

function parseLearned(fn: FunctionId, list: unknown): LearnedId[] {
  if (!Array.isArray(list)) return bad(`learned.${fn}`)
  const seen = new Set<number>()
  return list.map((e: unknown): LearnedId => {
    if (!isObj(e) || typeof e.name !== 'string' || nameBytes(e.name) > NAME_MAX_BYTES) return bad(`learned.${fn} entry`)
    if (!validId(fn, e.id)) return bad(`learned.${fn} id`)
    const id = e.id as number
    if (seen.has(id)) return bad(`learned.${fn} duplicate id`)
    seen.add(id)
    const out: LearnedId = { id, name: e.name }
    if (e.text !== undefined) {
      if (fn !== 6 || typeof e.text !== 'string' || e.text.length > MAX_QR_TEXT) return bad(`learned.${fn} text`)
      out.text = e.text
    }
    const c = e.color
    if (c === undefined) return out
    if (!(Array.isArray(c) && c.length === 3 && c.every((x) => isInt(x, 0, 255)))) return bad('learned color')
    return { ...out, color: [c[0], c[1], c[2]] }
  })
}

/** Parses and validates; throws Error on anything malformed or out of range. */
export function parseConfig(json: string): Config {
  let raw: unknown
  try {
    raw = JSON.parse(json)
  } catch {
    return bad('not JSON')
  }
  if (!isObj(raw) || raw.version !== 1) return bad('version')
  const { learned, params, system } = raw
  if (!isObj(learned)) return bad('learned')
  const out = emptyLearned()
  for (const fn of FNS) out[fn] = parseLearned(fn, learned[fn])
  for (const fn of OPTIONAL_FNS) if (learned[fn] !== undefined) out[fn] = parseLearned(fn, learned[fn])
  if (!isObj(params) || !isObj(params.color) || !isObj(params.tag) || !isObj(params.face)) return bad('params')
  const { color: c, tag: t, face: f } = params
  if (!inRange(c.tolerance, 0, 100) || !isInt(c.minArea, 0) || !isInt(c.maxResults, 1, 8) || !isBool(c.showNames))
    return bad('params.color')
  if (!isBool(t.showNames)) return bad('params.tag')
  if (!inRange(f.threshold, 0, 100) || !isInt(f.maxResults, 1, 8)) return bad('params.face')
  let gesture: FunctionParams['gesture'] | undefined // optional: files from before M8a have none; absent leaves the board alone
  if (params.gesture !== undefined) {
    const g = params.gesture
    if (!isObj(g) || !isInt(g.handThreshold, 10, 95) || !isInt(g.gestureThreshold, 0, 100) || (g.maxHands !== 1 && g.maxHands !== 2))
      return bad('params.gesture')
    gesture = { handThreshold: g.handThreshold, gestureThreshold: g.gestureThreshold, maxHands: g.maxHands }
  }
  let line: FunctionParams['line'] | undefined
  if (params.line !== undefined) {
    const l = params.line
    if (
      !isObj(l) ||
      !(l.mode === 'dark' || l.mode === 'light' || l.mode === 'learned') ||
      !isInt(l.threshold, 0, 255) ||
      !isInt(l.roiPercent, 20, 100) ||
      !isInt(l.bands, 4, 12) ||
      !isInt(l.minWidth, 1, 100) ||
      !isInt(l.maxWidth, l.minWidth, 255) ||
      !isInt(l.colorTolerance, 0, 100)
    )
      return bad('params.line')
    line = { mode: l.mode, threshold: l.threshold, roiPercent: l.roiPercent, bands: l.bands, minWidth: l.minWidth, maxWidth: l.maxWidth, colorTolerance: l.colorTolerance }
  }
  let track: FunctionParams['track'] | undefined
  if (params.track !== undefined) {
    const k = params.track
    if (!isObj(k) || !isInt(k.threshold, 10, 95) || !isInt(k.adapt, 0, 50) || !isBool(k.reacquire) || !isBool(k.multiScale)) return bad('params.track')
    track = { threshold: k.threshold, adapt: k.adapt, reacquire: k.reacquire, multiScale: k.multiScale }
  }
  let shape: FunctionParams['shape'] | undefined // optional: files from before M7 have none
  if (params.shape !== undefined) {
    const s = params.shape
    if (
      !isObj(s) ||
      (s.mode !== 'dark' && s.mode !== 'light') ||
      !isInt(s.threshold, 0, 255) ||
      !isInt(s.minSize, 16, 240) ||
      !isInt(s.maxResults, 1, 8)
    )
      return bad('params.shape')
    shape = { mode: s.mode, threshold: s.threshold, minSize: s.minSize, maxResults: s.maxResults }
  }
  let motion: FunctionParams['motion'] | undefined
  if (params.motion !== undefined) {
    const m = params.motion
    if (!isObj(m) || !isInt(m.threshold, 5, 100) || !isInt(m.minSize, 8, 240) || !isInt(m.maxResults, 1, 8)) return bad('params.motion')
    motion = { threshold: m.threshold, minSize: m.minSize, maxResults: m.maxResults }
  }
  if (!isObj(system)) return bad('system')
  const { uartBaud, i2cAddress, ledBrightness } = system
  if ((uartBaud !== 115200 && uartBaud !== 9600) || !isValidI2cAddress(i2cAddress) || !inRange(ledBrightness, 0, 100))
    return bad('system')
  return {
    learned: out,
    params: {
      color: { tolerance: c.tolerance, minArea: c.minArea, maxResults: c.maxResults, showNames: c.showNames },
      tag: { showNames: t.showNames },
      face: { threshold: f.threshold, maxResults: f.maxResults },
      ...(gesture ? { gesture } : {}),
      ...(line ? { line } : {}),
      ...(track ? { track } : {}),
      ...(shape ? { shape } : {}),
      ...(motion ? { motion } : {}),
    },
    system: { uartBaud, i2cAddress, ledBrightness },
  }
}
