import vectors from "../../../docs/protocol-vectors.json";

export interface FrameVector { name: string; ver: number; cmd: number; seq: number; payload_hex: string; encoded_hex: string }
export interface DecoderVector { name: string; chunks_hex: string[]; expect: { cmd: number; seq: number; payload_hex: string }[] }

export const crcVectors = vectors.crc16 as { input_ascii?: string; input_hex?: string; crc: string }[];
export const frameVectors = vectors.frames as FrameVector[];
export const decoderVectors = vectors.decoder as DecoderVector[];

export function fromHex(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}
export function toHex(b: Uint8Array): string {
  return Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
}
export function frameVector(name: string): FrameVector {
  const v = frameVectors.find((f) => f.name === name);
  if (!v) throw new Error(`no vector ${name}`);
  return v;
}
