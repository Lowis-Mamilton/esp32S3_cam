import { describe, expect, it } from "vitest";
import { Decoder, crc16, encodeFrame } from "./codec";
import { crcVectors, decoderVectors, frameVectors, fromHex, toHex } from "./vectors";

describe("crc16", () => {
  for (const v of crcVectors) {
    it(`matches ${v.input_ascii ?? v.input_hex}`, () => {
      const input = v.input_ascii !== undefined ? new TextEncoder().encode(v.input_ascii) : fromHex(v.input_hex!);
      expect(crc16(input).toString(16).padStart(4, "0")).toBe(v.crc);
    });
  }
});

describe("encodeFrame", () => {
  for (const v of frameVectors) {
    it(v.name, () => {
      expect(toHex(encodeFrame(v.cmd, v.seq, fromHex(v.payload_hex)))).toBe(v.encoded_hex);
    });
  }
  it("defaults to empty payload", () => {
    expect(toHex(encodeFrame(2, 1))).toBe("4d5801020100000561");
  });
});

describe("Decoder", () => {
  for (const v of decoderVectors) {
    it(v.name, () => {
      const d = new Decoder();
      const got = v.chunks_hex.flatMap((c) => d.push(fromHex(c)));
      expect(got.map((f) => ({ cmd: f.cmd, seq: f.seq, payload_hex: toHex(f.payload) }))).toEqual(v.expect);
    });
  }

  it("drops oversize len and resumes at the next byte", () => {
    const d = new Decoder(4);
    const hdr = fromHex("4d580102ff0500");
    const got = d.push(new Uint8Array([...hdr, ...encodeFrame(2, 1)]));
    expect(got.map((f) => f.seq)).toEqual([1]);
  });

  it("returns independent payload copies", () => {
    const d = new Decoder();
    const [a] = d.push(encodeFrame(0x60, 0, new Uint8Array([1, 2, 3])));
    d.push(encodeFrame(0x60, 1, new Uint8Array([9, 9, 9])));
    expect(Array.from(a.payload)).toEqual([1, 2, 3]);
  });

  it("treats 0x4D while awaiting magic1 as a new magic0", () => {
    const d = new Decoder();
    const got = d.push(new Uint8Array([0x4d, ...encodeFrame(2, 1)]));
    expect(got).toHaveLength(1);
  });
});
