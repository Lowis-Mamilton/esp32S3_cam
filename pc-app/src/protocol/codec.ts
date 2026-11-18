// MXCam Protocol v1 framing. See docs/protocol.md §1-§2.

const MAGIC0 = 0x4d;
const MAGIC1 = 0x58;
const VERSION = 0x01;
const HEADER_LEN = 7;
const MAX_U16 = 0xffff;

export interface Frame {
  cmd: number;
  seq: number;
  payload: Uint8Array;
}

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR. */
export function crc16(data: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let i = 0; i < 8; i++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export function encodeFrame(cmd: number, seq: number, payload: Uint8Array = new Uint8Array(0)): Uint8Array {
  if (payload.length > MAX_U16) throw new RangeError(`payload too long: ${payload.length}`);
  const out = new Uint8Array(HEADER_LEN + payload.length + 2);
  out[0] = MAGIC0;
  out[1] = MAGIC1;
  out[2] = VERSION;
  out[3] = cmd & 0xff;
  out[4] = seq & 0xff;
  out[5] = payload.length & 0xff;
  out[6] = payload.length >> 8;
  out.set(payload, HEADER_LEN);
  const crc = crc16(out.subarray(2, HEADER_LEN + payload.length));
  out[HEADER_LEN + payload.length] = crc & 0xff;
  out[HEADER_LEN + payload.length + 1] = crc >> 8;
  return out;
}

/**
 * Streaming decoder. Keeps a buffer of bytes that may still start a frame and
 * rescans from the byte after a failed candidate, so garbage never swallows
 * valid frames that follow it.
 */
export class Decoder {
  private buf = new Uint8Array(0);

  private readonly maxPayload: number;

  constructor(maxPayload = 65535) {
    this.maxPayload = maxPayload;
  }

  push(chunk: Uint8Array): Frame[] {
    const merged = new Uint8Array(this.buf.length + chunk.length);
    merged.set(this.buf);
    merged.set(chunk, this.buf.length);
    const buf = merged;

    const frames: Frame[] = [];
    let pos = 0;
    while (pos < buf.length) {
      if (buf[pos] !== MAGIC0) {
        pos++;
        continue;
      }
      if (pos + 1 >= buf.length) break; // need magic1
      if (buf[pos + 1] !== MAGIC1) {
        pos++; // a 0x4D at pos+1 is re-examined as magic0 on the next iteration
        continue;
      }
      if (pos + HEADER_LEN > buf.length) break; // need rest of header
      const len = buf[pos + 5] | (buf[pos + 6] << 8);
      if (buf[pos + 2] !== VERSION || len > this.maxPayload) {
        pos++;
        continue;
      }
      const total = HEADER_LEN + len + 2;
      if (pos + total > buf.length) break; // need rest of frame
      const end = pos + HEADER_LEN + len;
      const expected = buf[end] | (buf[end + 1] << 8);
      if (crc16(buf.subarray(pos + 2, end)) !== expected) {
        pos++;
        continue;
      }
      frames.push({
        cmd: buf[pos + 3],
        seq: buf[pos + 4],
        payload: buf.slice(pos + HEADER_LEN, end),
      });
      pos += total;
    }
    this.buf = buf.slice(pos);
    return frames;
  }
}
