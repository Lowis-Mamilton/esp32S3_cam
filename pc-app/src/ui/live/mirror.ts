import { useSyncExternalStore } from 'react'

const MIRROR_KEY = 'mxcam.mirror'
const listeners = new Set<() => void>()

/** Mirror view (ADR-0010) is a per-browser display preference, on by default. */
function readMirror(): boolean {
  try {
    return localStorage.getItem(MIRROR_KEY) !== '0'
  } catch {
    return memory
  }
}
let memory = true // used while storage is unavailable
function storeMirror(on: boolean) {
  memory = on
  try {
    localStorage.setItem(MIRROR_KEY, on ? '1' : '0')
  } catch {
    // storage unavailable: the in-memory value still applies
  }
  listeners.forEach((l) => l())
}
const subscribe = (cb: () => void) => {
  listeners.add(cb)
  return () => void listeners.delete(cb)
}

/** The mirror-view preference, shared by the live stage (toggle) and the results tab (hint). */
export function useMirror(): [boolean, (on: boolean) => void] {
  return [useSyncExternalStore(subscribe, readMirror, () => true), storeMirror]
}
