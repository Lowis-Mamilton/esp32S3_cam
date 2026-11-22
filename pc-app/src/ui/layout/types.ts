/** Functions the UI has screens for; same values as protocol FunctionId (Color, AprilTag, Face, Line, Track, Gesture). */
export type FunctionId = 1 | 2 | 3 | 4 | 5 | 8
/**
 * Every function the device layer drives: FunctionId plus M7's QR code (6), shapes (11) and motion (12).
 * Staging alias: once the UI has screens for 6, 11 and 12 they are added to FunctionId and this equals it.
 */
export type DeviceFunctionId = FunctionId | 6 | 11 | 12
export type View = { kind: 'function'; id: FunctionId } | { kind: 'settings' } | { kind: 'firmware' }
export type PanelTab = 'results' | 'learn' | 'params' | 'code'
