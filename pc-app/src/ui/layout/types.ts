/** Same values as protocol FunctionId (Color, AprilTag, Face). */
export type FunctionId = 1 | 2 | 3
export type View = { kind: 'function'; id: FunctionId } | { kind: 'settings' } | { kind: 'firmware' }
export type PanelTab = 'results' | 'learn' | 'params' | 'code'
