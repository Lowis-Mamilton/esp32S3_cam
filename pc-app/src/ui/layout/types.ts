/** Same values as protocol FunctionId (Color, AprilTag, Face, Line, Track, Gesture). */
export type FunctionId = 1 | 2 | 3 | 4 | 5 | 8
export type View = { kind: 'function'; id: FunctionId } | { kind: 'settings' } | { kind: 'firmware' }
export type PanelTab = 'results' | 'learn' | 'params' | 'code'
