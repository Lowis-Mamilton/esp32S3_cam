// MXCam Protocol v1 message layouts. See docs/protocol.md §3-§6.

export const Cmd = {
  GetInfo: 0x01,
  Ping: 0x02,
  SetFunction: 0x10,
  GetFunction: 0x11,
  GetResults: 0x20,
  Learn: 0x30,
  Forget: 0x31,
  SetName: 0x32,
  GetLearned: 0x33,
  GetConfig: 0x40,
  SetConfig: 0x41,
  Save: 0x42,
  FactoryReset: 0x43,
  StreamStart: 0x50,
  StreamStop: 0x51,
  PushFrame: 0x60,
  PushResults: 0x61,
  Error: 0xff,
} as const;

export const responseOf = (cmd: number): number => cmd | 0x80;

export const ErrorCode = {
  UnknownCmd: 1,
  BadLength: 2,
  BadParam: 3,
  NotSupported: 4,
  Busy: 5,
  Internal: 6,
  Full: 7,
} as const;

export const FunctionId = { None: 0, Color: 1, AprilTag: 2, Face: 3 } as const;

export interface DeviceInfo {
  protocolVersion: number;
  firmwareVersion: string;
  firmwareGit: string;
  serial: string;
  frameWidth: number;
  frameHeight: number;
  maxResults: number;
  functionIds: number[];
}

const INFO_FIXED_LEN = 23;

function view(p: Uint8Array): DataView {
  return new DataView(p.buffer, p.byteOffset, p.byteLength);
}

export function parseInfo(payload: Uint8Array): DeviceInfo {
  if (payload.length < INFO_FIXED_LEN) {
    throw new Error(`GET_INFO payload too short: ${payload.length}`);
  }
  const v = view(payload);
  const functionCount = payload[22];
  if (payload.length !== INFO_FIXED_LEN + functionCount) {
    throw new Error(`GET_INFO function_count ${functionCount} does not match payload length ${payload.length}`);
  }
  const serial = Array.from(payload.subarray(11, 17), (b) => b.toString(16).padStart(2, "0").toUpperCase()).join(":");
  return {
    protocolVersion: payload[0],
    firmwareVersion: `${payload[1]}.${payload[2]}.${payload[3]}`,
    firmwareGit: String.fromCharCode(...payload.subarray(4, 11)),
    serial,
    frameWidth: v.getUint16(17, true),
    frameHeight: v.getUint16(19, true),
    maxResults: payload[21],
    functionIds: Array.from(payload.subarray(INFO_FIXED_LEN)),
  };
}

export function encodeStreamStart(o: {
  frames: boolean;
  results: boolean;
  maxFps: number;
  jpegQuality: number;
}): Uint8Array {
  return new Uint8Array([(o.frames ? 1 : 0) | (o.results ? 2 : 0), o.maxFps & 0xff, o.jpegQuality & 0xff]);
}

export interface FramePush {
  frameId: number;
  timestampMs: number;
  width: number;
  height: number;
  jpeg: Uint8Array;
}

const FRAME_PUSH_HEADER_LEN = 12;

export function parseFramePush(payload: Uint8Array): FramePush {
  if (payload.length < FRAME_PUSH_HEADER_LEN) {
    throw new Error(`FRAME payload too short: ${payload.length}`);
  }
  const v = view(payload);
  return {
    frameId: v.getUint32(0, true),
    timestampMs: v.getUint32(4, true),
    width: v.getUint16(8, true),
    height: v.getUint16(10, true),
    jpeg: payload.slice(FRAME_PUSH_HEADER_LEN),
  };
}

export function parseError(payload: Uint8Array): { cmd: number; code: number } {
  if (payload.length < 2) throw new Error(`ERROR payload too short: ${payload.length}`);
  return { cmd: payload[0], code: payload[1] };
}

// --- M2 (docs/protocol.md section 7) ---

export const MAX_RESULTS = 8;
export const NAME_MAX_BYTES = 48;
const RESULTS_FIXED_LEN = 6;
const RECORD_FIXED_LEN = 12;
const TAG_EXTRA_LEN = 18;
const CONFIG_LEN = 18;

export interface ResultRecord {
  id: number;
  /** Box centre, 320x240 coordinates. */
  x: number;
  y: number;
  width: number;
  height: number;
  /** 0-100; undefined when the device sent 255 (not applicable). */
  confidence: number | undefined;
  /** AprilTag only. rotationDeg is degrees (rotation_cdeg / 100). */
  extra?: { rotationDeg: number; corners: [number, number][] };
}

export interface ResultsPush {
  frameId: number;
  functionId: number;
  results: ResultRecord[];
}

/** GET_RESULTS response (0xA0) and PUSH_RESULTS (0x61) payload. */
export function parseResults(payload: Uint8Array): ResultsPush {
  if (payload.length < RESULTS_FIXED_LEN) throw new Error(`RESULTS payload too short: ${payload.length}`);
  const v = view(payload);
  const count = payload[5];
  if (count > MAX_RESULTS) throw new Error(`RESULTS count ${count} exceeds ${MAX_RESULTS}`);
  const results: ResultRecord[] = [];
  let o = RESULTS_FIXED_LEN;
  for (let i = 0; i < count; i++) {
    if (o + RECORD_FIXED_LEN > payload.length) throw new Error("RESULTS payload truncated");
    const extraLen = payload[o + 11];
    if (extraLen !== 0 && extraLen !== TAG_EXTRA_LEN) throw new Error(`RESULTS extra_len ${extraLen} is invalid`);
    if (o + RECORD_FIXED_LEN + extraLen > payload.length) throw new Error("RESULTS payload truncated");
    const rec: ResultRecord = {
      id: v.getUint16(o, true),
      x: v.getInt16(o + 2, true),
      y: v.getInt16(o + 4, true),
      width: v.getUint16(o + 6, true),
      height: v.getUint16(o + 8, true),
      confidence: payload[o + 10] === 255 ? undefined : payload[o + 10],
    };
    if (extraLen === TAG_EXTRA_LEN) {
      const e = o + RECORD_FIXED_LEN;
      const corners: [number, number][] = [];
      for (let k = 0; k < 4; k++) corners.push([v.getInt16(e + 2 + 4 * k, true), v.getInt16(e + 4 + 4 * k, true)]);
      rec.extra = { rotationDeg: v.getInt16(e, true) / 100, corners };
    }
    results.push(rec);
    o += RECORD_FIXED_LEN + extraLen;
  }
  if (o !== payload.length) throw new Error(`RESULTS has ${payload.length - o} trailing bytes`);
  return { frameId: v.getUint32(0, true), functionId: payload[4], results };
}

/** LEARN request: rect is the top-left corner and size, 320x240 coordinates. */
export function encodeLearn(
  functionId: number,
  rect: { x: number; y: number; width: number; height: number },
): Uint8Array {
  const out = new Uint8Array(9);
  const v = view(out);
  out[0] = functionId;
  v.setUint16(1, rect.x, true);
  v.setUint16(3, rect.y, true);
  v.setUint16(5, rect.width, true);
  v.setUint16(7, rect.height, true);
  return out;
}

/** FORGET request; id 0xFFFF forgets everything. */
export function encodeForget(functionId: number, id: number): Uint8Array {
  const out = new Uint8Array(3);
  out[0] = functionId;
  view(out).setUint16(1, id, true);
  return out;
}

/** SET_NAME request. An empty name clears it. Throws when the UTF-8 form exceeds 48 bytes. */
export function encodeSetName(functionId: number, id: number, name: string): Uint8Array {
  const bytes = new TextEncoder().encode(name);
  if (bytes.length > NAME_MAX_BYTES) throw new Error(`name is ${bytes.length} bytes in UTF-8, max ${NAME_MAX_BYTES}`);
  const out = new Uint8Array(4 + bytes.length);
  out[0] = functionId;
  view(out).setUint16(1, id, true);
  out[3] = bytes.length;
  out.set(bytes, 4);
  return out;
}

export interface LearnedItem {
  id: number;
  rgb: [number, number, number];
  name: string;
}

/** GET_LEARNED response (0xB3). */
export function parseLearned(payload: Uint8Array): LearnedItem[] {
  if (payload.length < 1) throw new Error("GET_LEARNED payload empty");
  const v = view(payload);
  const decoder = new TextDecoder("utf-8", { fatal: true });
  const items: LearnedItem[] = [];
  let o = 1;
  for (let i = 0; i < payload[0]; i++) {
    if (o + 6 > payload.length) throw new Error("GET_LEARNED payload truncated");
    const nameLen = payload[o + 5];
    if (nameLen > NAME_MAX_BYTES || o + 6 + nameLen > payload.length) throw new Error("GET_LEARNED payload truncated");
    items.push({
      id: v.getUint16(o, true),
      rgb: [payload[o + 2], payload[o + 3], payload[o + 4]],
      name: decoder.decode(payload.subarray(o + 6, o + 6 + nameLen)),
    });
    o += 6 + nameLen;
  }
  if (o !== payload.length) throw new Error(`GET_LEARNED has ${payload.length - o} trailing bytes`);
  return items;
}

export interface DeviceConfig {
  tolerance: number;
  minArea: number;
  colorMaxResults: number;
  colorShowNames: boolean;
  tagShowNames: boolean;
  faceThreshold: number;
  faceMaxResults: number;
  uartBaud: number;
  i2cAddress: number;
  ledBrightness: number;
  /** Read-only: true when there are changes not yet saved to flash. Never sent to the device. */
  dirty: boolean;
}

/** Config v1 (18 bytes). dirty is read-only, so it is always written as 0. */
export function encodeConfig(c: DeviceConfig): Uint8Array {
  const out = new Uint8Array(CONFIG_LEN);
  const v = view(out);
  out[0] = 1;
  out[1] = c.tolerance;
  v.setUint16(2, c.minArea, true);
  out[4] = c.colorMaxResults;
  out[5] = c.colorShowNames ? 1 : 0;
  out[6] = c.tagShowNames ? 1 : 0;
  out[7] = c.faceThreshold;
  out[8] = c.faceMaxResults;
  v.setUint32(9, c.uartBaud, true);
  out[13] = c.i2cAddress;
  out[14] = c.ledBrightness;
  return out;
}

export function parseConfig(payload: Uint8Array): DeviceConfig {
  if (payload.length !== CONFIG_LEN) throw new Error(`CONFIG payload must be ${CONFIG_LEN} bytes, got ${payload.length}`);
  if (payload[0] !== 1) throw new Error(`unsupported config_version ${payload[0]}`);
  const v = view(payload);
  return {
    tolerance: payload[1],
    minArea: v.getUint16(2, true),
    colorMaxResults: payload[4],
    colorShowNames: payload[5] !== 0,
    tagShowNames: payload[6] !== 0,
    faceThreshold: payload[7],
    faceMaxResults: payload[8],
    uartBaud: v.getUint32(9, true),
    i2cAddress: payload[13],
    ledBrightness: payload[14],
    dirty: payload[15] !== 0,
  };
}
