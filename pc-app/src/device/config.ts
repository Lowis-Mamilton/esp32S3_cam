import { emptyLearned, type FunctionId, type FunctionParams, type LearnedId, type SystemConfig } from './types'

/** Versioned device configuration, as exported/imported by Studio. */
export interface Config {
  learned: Record<FunctionId, LearnedId[]>
  params: FunctionParams
  system: SystemConfig
}

const FNS: FunctionId[] = [1, 2, 3]
const MAX_NAME_LEN = 32
const MAX_TAG_ID = 586 // tag36h11 has 587 codes

const isObj = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v)
const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const isInt = (v: unknown, min: number, max = Infinity): v is number => Number.isInteger(v) && (v as number) >= min && (v as number) <= max
const inRange = (v: unknown, min: number, max: number): v is number => isNum(v) && v >= min && v <= max
const isBool = (v: unknown): v is boolean => typeof v === 'boolean'

export const I2C_MIN = 0x08
export const I2C_MAX = 0x77
export const isValidI2cAddress = (v: unknown): v is number => isInt(v, I2C_MIN, I2C_MAX)

function bad(what: string): never {
  throw new Error(`invalid config: ${what}`)
}

function parseLearned(fn: FunctionId, list: unknown): LearnedId[] {
  if (!Array.isArray(list)) return bad(`learned.${fn}`)
  const seen = new Set<number>()
  return list.map((e: unknown): LearnedId => {
    if (!isObj(e) || typeof e.name !== 'string' || e.name.length > MAX_NAME_LEN) return bad(`learned.${fn} entry`)
    // color: 1-7, tag: 0-586, face: 1-7 (same slots as colour)
    const ok = fn === 2 ? isInt(e.id, 0, MAX_TAG_ID) : isInt(e.id, 1, 7)
    if (!ok) return bad(`learned.${fn} id`)
    const id = e.id as number
    if (seen.has(id)) return bad(`learned.${fn} duplicate id`)
    seen.add(id)
    const c = e.color
    if (c === undefined) return { id, name: e.name }
    if (!(Array.isArray(c) && c.length === 3 && c.every((x) => isInt(x, 0, 255)))) return bad('learned color')
    return { id, name: e.name, color: [c[0], c[1], c[2]] }
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
  if (!isObj(params) || !isObj(params.color) || !isObj(params.tag) || !isObj(params.face)) return bad('params')
  const { color: c, tag: t, face: f } = params
  if (!inRange(c.tolerance, 0, 100) || !isInt(c.minArea, 0) || !isInt(c.maxResults, 1, 8) || !isBool(c.showNames))
    return bad('params.color')
  if (!isBool(t.showNames)) return bad('params.tag')
  if (!inRange(f.threshold, 0, 100) || !isInt(f.maxResults, 1, 8)) return bad('params.face')
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
    },
    system: { uartBaud, i2cAddress, ledBrightness },
  }
}
