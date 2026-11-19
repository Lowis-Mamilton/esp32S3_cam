import { useCallback, useState } from 'react'
import { NotSupportedError, type Device } from '../../device/types'

export interface Guard {
  /** The device refused an operation with NotSupportedError. */
  needFirmware: boolean
  error: string | null
  /** Runs a device operation; never rejects. */
  run(op: () => Promise<unknown>): Promise<void>
}

interface State {
  device: Device | null
  needFirmware: boolean
  error: string | null
}

const isNotSupported = (e: unknown) => e instanceof NotSupportedError || (e instanceof Error && e.name === 'NotSupportedError')

/** Turns device failures into inline state. State belongs to one device instance. */
export function useGuard(device: Device | null): Guard {
  const [state, setState] = useState<State>({ device, needFirmware: false, error: null })
  const run = useCallback(
    async (op: () => Promise<unknown>) => {
      try {
        await op()
        setState((s) => (s.error ? { ...s, error: null } : s))
      } catch (e) {
        const nf = isNotSupported(e)
        setState((s) => ({
          device,
          needFirmware: nf || (s.device === device && s.needFirmware),
          error: nf ? null : e instanceof Error ? e.message : String(e),
        }))
      }
    },
    [device],
  )
  const mine = state.device === device
  return { needFirmware: mine && state.needFirmware, error: mine ? state.error : null, run }
}
