/**
 * Every function Studio knows; same values as protocol FunctionId
 * (Color, AprilTag, Face, Line, Track, QR code, Gesture, Shapes, Motion).
 */
export type FunctionId = 1 | 2 | 3 | 4 | 5 | 6 | 8 | 11 | 12
export type View = { kind: 'function'; id: FunctionId } | { kind: 'settings' } | { kind: 'firmware' }
export type PanelTab = 'results' | 'learn' | 'params' | 'code'
