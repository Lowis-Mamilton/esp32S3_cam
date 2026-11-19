import { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react'
import { DemoDevice } from './DemoDevice'
import { RealDevice } from './RealDevice'
import { defaultParams, defaultSystem, emptyLearned, type Device, type DeviceSnapshot } from './types'

const STORAGE_KEY = 'mxcam.demo'

function readDemo(): boolean {
  try {
    return localStorage.getItem(STORAGE_KEY) === '1'
  } catch {
    return false
  }
}

function idleSnapshot(demo: boolean, serial: Serial | undefined): DeviceSnapshot {
  return {
    mode: demo ? 'demo' : 'real',
    status: serial ? 'disconnected' : 'unsupported',
    supported: [],
    activeFunction: 0,
    learned: emptyLearned(),
    params: defaultParams(),
    system: defaultSystem(),
    dirty: false,
    lost: false,
    configKnown: demo,
    previewPaused: false,
  }
}

const noopSubscribe = () => () => {}

export interface UseDevice {
  /** null for the single render before the device is created. */
  device: Device | null
  snapshot: DeviceSnapshot
  demo: boolean
  setDemo(on: boolean): void
}

/** Creates the Device for the current mode; switching disposes the old one first. */
export function useDevice(serial: Serial | undefined): UseDevice {
  const [demo, setDemoState] = useState(readDemo)
  const [device, setDevice] = useState<Device | null>(null)

  useEffect(() => {
    const d = demo ? new DemoDevice() : new RealDevice(serial)
    // oxlint-disable-next-line react/set-state-in-effect -- publishes the externally owned device instance
    setDevice(d)
    return () => {
      d.dispose()
      setDevice(null)
    }
  }, [demo, serial])

  const subscribe = useMemo(() => (device ? (cb: () => void) => device.subscribe(cb) : noopSubscribe), [device])
  const snapshot = useSyncExternalStore(
    subscribe,
    device ? () => device.snapshot() : () => IDLE_CACHE.get(demo, serial),
  )

  const setDemo = useCallback((on: boolean) => {
    setDemoState(on)
    try {
      localStorage.setItem(STORAGE_KEY, on ? '1' : '0')
    } catch {
      // storage unavailable
    }
  }, [])

  return { device, snapshot, demo, setDemo }
}

/** useSyncExternalStore needs a stable reference while nothing changed. */
const IDLE_CACHE = {
  key: '' as string,
  value: null as DeviceSnapshot | null,
  get(demo: boolean, serial: Serial | undefined): DeviceSnapshot {
    const key = `${demo}:${serial ? 1 : 0}`
    if (this.key !== key || !this.value) {
      this.key = key
      this.value = idleSnapshot(demo, serial)
    }
    return this.value
  },
}
