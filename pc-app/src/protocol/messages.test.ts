import { describe, expect, it } from "vitest";
import { Cmd, encodeStreamStart, parseError, parseFramePush, parseInfo, responseOf } from "./messages";
import { frameVector, fromHex, toHex } from "./vectors";

describe("messages", () => {
  it("responseOf sets bit 7", () => {
    expect(responseOf(Cmd.GetInfo)).toBe(0x81);
    expect(responseOf(Cmd.StreamStart)).toBe(0xd0);
  });

  it("parseInfo", () => {
    const info = parseInfo(fromHex(frameVector("get_info_response_m1").payload_hex));
    expect(info).toEqual({
      protocolVersion: 1, firmwareVersion: "0.1.0", firmwareGit: "a1b2c3d",
      serial: "24:EC:4A:00:11:FF", frameWidth: 320, frameHeight: 240, maxResults: 8, functionIds: [],
    });
  });

  it("parseInfo reads function ids", () => {
    const p = fromHex(frameVector("get_info_response_m1").payload_hex);
    const q = new Uint8Array([...p.slice(0, p.length - 1), 2, 1, 2]);
    expect(parseInfo(q).functionIds).toEqual([1, 2]);
  });

  it("parseInfo throws on short payload", () => {
    expect(() => parseInfo(new Uint8Array(5))).toThrow();
  });

  it("parseInfo throws on function_count mismatch", () => {
    const p = fromHex(frameVector("get_info_response_m1").payload_hex);
    const q = new Uint8Array([...p.slice(0, p.length - 1), 3, 1]);
    expect(() => parseInfo(q)).toThrow();
  });

  it("encodeStreamStart", () => {
    const got = encodeStreamStart({ frames: true, results: false, maxFps: 10, jpegQuality: 0 });
    expect(toHex(got)).toBe(frameVector("stream_start_frames_10fps_default_quality").payload_hex);
    expect(encodeStreamStart({ frames: true, results: true, maxFps: 5, jpegQuality: 80 })[0]).toBe(3);
  });

  it("parseFramePush", () => {
    const f = parseFramePush(fromHex(frameVector("frame_push").payload_hex));
    expect(f).toMatchObject({ frameId: 1, timestampMs: 1000, width: 320, height: 240 });
    expect(toHex(f.jpeg)).toBe("ffd8ffd9");
  });

  it("parseFramePush throws on short payload", () => {
    expect(() => parseFramePush(new Uint8Array(11))).toThrow();
  });

  it("parseError", () => {
    expect(parseError(fromHex(frameVector("error_unknown_command").payload_hex))).toEqual({ cmd: 0x7e, code: 1 });
  });

  it("parseError throws on short payload", () => {
    expect(() => parseError(new Uint8Array(1))).toThrow();
  });
});
