// MXCam Protocol v1 message layouts. See docs/protocol.md §3-§6.

export const Cmd = {
  GetInfo: 0x01,
  Ping: 0x02,
  SetFunction: 0x10,
  GetFunction: 0x11,
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
