import { describe, expect, it } from "vitest";
import {
  Cmd, ErrorCode, FunctionId, Gesture, Shape, encodeConfig, encodeForget, encodeFuncConfig, encodeGetFuncConfig,
  encodeGetLearnedData, encodeGetResultData, encodeLearn, encodeSetName, encodeStreamStart, parseConfig, parseError,
  parseFramePush, parseFuncConfig, parseInfo, parseLearned, parseLearnedData, parseResultData, parseResults, responseOf,
} from "./messages";
import { frameVector, fromHex, payloadVectors as pv, toHex } from "./vectors";

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

  it("M2 command codes and FULL error", () => {
    expect([Cmd.GetResults, Cmd.Learn, Cmd.Forget, Cmd.SetName, Cmd.GetLearned, Cmd.GetConfig, Cmd.SetConfig]).toEqual([
      0x20, 0x30, 0x31, 0x32, 0x33, 0x40, 0x41,
    ]);
    expect(ErrorCode.Full).toBe(7);
  });
});

describe("parseResults", () => {
  it("colour records", () => {
    const r = parseResults(fromHex(frameVector("results_color_two").payload_hex));
    expect(r).toEqual({
      frameId: 42, functionId: 1,
      results: [
        { id: 1, x: 100, y: 80, width: 40, height: 30, confidence: undefined },
        { id: 2, x: 250, y: 200, width: 20, height: 18, confidence: undefined },
      ],
    });
    expect(r.results.length).toBe(pv.results_color_two.results.length);
  });

  it("tag record with extra", () => {
    const r = parseResults(fromHex(frameVector("results_tag_one").payload_hex));
    const e = pv.results_tag_one.results[0];
    expect(r.frameId).toBe(pv.results_tag_one.frame_id);
    expect(r.functionId).toBe(2);
    expect(r.results).toHaveLength(1);
    expect(r.results[0]).toMatchObject({ id: 3, x: 160, y: 120, width: 52, height: 50, confidence: undefined });
    expect(r.results[0].extra).toEqual({ rotationDeg: 45, corners: e.extra!.corners });
  });

  it("keeps confidence when not 255 and handles empty", () => {
    const p = fromHex(frameVector("results_color_two").payload_hex);
    p[6 + 10] = 87;
    expect(parseResults(p).results[0].confidence).toBe(87);
    expect(parseResults(new Uint8Array([1, 0, 0, 0, 3, 0]))).toEqual({ frameId: 1, functionId: 3, results: [] });
  });

  it("negative coordinates and rotation", () => {
    const p = fromHex(frameVector("results_tag_one").payload_hex);
    const v = new DataView(p.buffer);
    v.setInt16(6 + 2, -5, true);
    v.setInt16(6 + 12, -18000, true);
    const r = parseResults(p).results[0];
    expect(r.x).toBe(-5);
    expect(r.extra!.rotationDeg).toBe(-180);
  });

  it("throws on count > 8, bad extra_len, truncation and trailing bytes", () => {
    const base = fromHex(frameVector("results_color_two").payload_hex);
    const c9 = base.slice(); c9[5] = 9;
    expect(() => parseResults(c9)).toThrow();
    const ex = base.slice(); ex[6 + 11] = 5;
    expect(() => parseResults(ex)).toThrow();
    const ex19 = fromHex(frameVector("results_tag_one").payload_hex); ex19[6 + 11] = 19;
    expect(() => parseResults(ex19)).toThrow();
    expect(() => parseResults(base.slice(0, base.length - 1))).toThrow();
    expect(() => parseResults(new Uint8Array([...base, 0]))).toThrow();
    expect(() => parseResults(new Uint8Array(5))).toThrow();
  });
});

describe("learn / forget / name", () => {
  it("encodeLearn", () => {
    const v = pv.learn_request;
    expect(toHex(encodeLearn(v.function_id, { x: v.x, y: v.y, width: v.width, height: v.height }))).toBe(
      frameVector("learn_request").payload_hex,
    );
  });

  it("encodeForget", () => {
    expect(toHex(encodeForget(1, 3))).toBe("010300");
    expect(toHex(encodeForget(1, 0xffff))).toBe("01ffff");
  });

  it("encodeSetName with CJK", () => {
    const v = pv.set_name_utf8;
    expect(toHex(encodeSetName(v.function_id, v.id, v.name))).toBe(frameVector("set_name_utf8").payload_hex);
  });

  it("encodeSetName empty clears and 48 bytes ok", () => {
    expect(toHex(encodeSetName(1, 2, ""))).toBe("01020000");
    expect(encodeSetName(1, 2, "紅".repeat(16))[3]).toBe(48);
  });

  it("encodeSetName throws over 48 UTF-8 bytes", () => {
    expect(() => encodeSetName(1, 2, "a".repeat(49))).toThrow();
    expect(() => encodeSetName(1, 2, "紅".repeat(17))).toThrow();
  });

  it("parseLearned", () => {
    expect(parseLearned(fromHex(frameVector("learned_list").payload_hex))).toEqual(
      pv.learned_list.items.map((i) => ({ id: i.id, rgb: i.rgb, name: i.name })),
    );
    expect(parseLearned(new Uint8Array([0]))).toEqual([]);
  });

  it("parseLearned throws on malformed", () => {
    const p = fromHex(frameVector("learned_list").payload_hex);
    expect(() => parseLearned(p.slice(0, p.length - 1))).toThrow();
    expect(() => parseLearned(new Uint8Array([...p, 0]))).toThrow();
    expect(() => parseLearned(new Uint8Array(0))).toThrow();
  });
});

describe("config", () => {
  const d = pv.config_default;
  const cfg = {
    tolerance: 30, minArea: 100, colorMaxResults: 4, colorShowNames: true, tagShowNames: true,
    faceThreshold: 60, faceMaxResults: 4, uartBaud: 115200, i2cAddress: 0x5a, ledBrightness: 50, dirty: false,
  };

  it("parseConfig", () => {
    expect(d.i2c_address).toBe(0x5a);
    expect(parseConfig(fromHex(frameVector("config_default").payload_hex))).toEqual(cfg);
  });

  it("encodeConfig round trips and ignores dirty as 0", () => {
    expect(toHex(encodeConfig(cfg))).toBe(frameVector("config_default").payload_hex);
    expect(encodeConfig({ ...cfg, dirty: true })[15]).toBe(0);
  });

  it("parseConfig reads dirty flag", () => {
    const p = fromHex(frameVector("config_default").payload_hex);
    p[15] = 1;
    expect(parseConfig(p).dirty).toBe(true);
  });

  it("parseConfig throws on bad length or version", () => {
    const p = fromHex(frameVector("config_default").payload_hex);
    expect(() => parseConfig(p.slice(0, 17))).toThrow();
    const q = p.slice(); q[0] = 2;
    expect(() => parseConfig(q)).toThrow();
  });
});

describe("M8a gesture", () => {
  it("parseInfo reads gesture function", () => {
    expect(parseInfo(fromHex(frameVector("get_info_response_gesture").payload_hex)).functionIds).toEqual([1, 2, 3, 8]);
    expect(FunctionId.Gesture).toBe(8);
  });

  it("encodeGetFuncConfig for gesture", () => {
    expect(frameVector("get_func_config_gesture_request").cmd).toBe(Cmd.GetFuncConfig);
    expect(toHex(encodeGetFuncConfig(FunctionId.Gesture))).toBe(frameVector("get_func_config_gesture_request").payload_hex);
  });

  it("gesture func config vectors round trip", () => {
    for (const [name, v] of [
      ["func_config_gesture_default", pv.gesture_config_default],
      ["set_func_config_gesture_request", pv.gesture_config_set],
    ] as const) {
      const c = parseFuncConfig(fromHex(frameVector(name).payload_hex));
      expect(c).toEqual({
        functionId: 8,
        gesture: { handThreshold: v.hand_threshold, gestureThreshold: v.gesture_threshold, maxHands: v.max_hands },
      });
      expect(toHex(encodeFuncConfig(c))).toBe(frameVector(name).payload_hex);
    }
    expect(frameVector("func_config_gesture_default").cmd).toBe(Cmd.GetFuncConfig | 0x80);
    expect(frameVector("set_func_config_gesture_request").cmd).toBe(Cmd.SetFuncConfig);
  });

  it("parseFuncConfig rejects wrong gesture version/length", () => {
    const g = fromHex(frameVector("func_config_gesture_default").payload_hex);
    expect(() => parseFuncConfig(g.slice(0, 9))).toThrow();
    expect(() => parseFuncConfig(new Uint8Array([...g, 0]))).toThrow();
    const v2 = g.slice(); v2[1] = 2;
    expect(() => parseFuncConfig(v2)).toThrow();
    const v0 = g.slice(); v0[1] = 0;
    expect(() => parseFuncConfig(v0)).toThrow();
  });

  it("parseResults decodes gesture records", () => {
    const r = parseResults(fromHex(frameVector("results_gesture_two").payload_hex));
    const e = pv.results_gesture_two;
    expect(r.frameId).toBe(e.frame_id);
    expect(r.functionId).toBe(FunctionId.Gesture);
    expect(r.results).toHaveLength(2);
    expect(r.results.map((x) => [x.id, x.x, x.y, x.width, x.height, x.confidence])).toEqual(
      e.results.map((x) => [x.id, x.x, x.y, x.width, x.height, x.confidence]));
    expect(r.results[0].id).toBe(Gesture.Ok);
    expect(r.results[1].id).toBe(Gesture.Unknown);
    expect([r.results[0].confidence, r.results[1].confidence]).toEqual([92, 81]);
    for (const x of r.results) {
      expect(x.extra).toBeUndefined();
      expect(x.line).toBeUndefined();
    }
  });

  it("Gesture ids match the protocol table", () => {
    expect(Gesture).toEqual({ Unknown: 0, One: 1, Two: 2, Three: 3, Four: 4, Five: 5, Like: 6, Ok: 7, Call: 8, Dislike: 9 });
    expect(Gesture.Like).toBe(6);
    expect(Gesture.Dislike).toBe(9);
  });
});

describe("M6 groundwork", () => {
  it("command and function ids", () => {
    expect([Cmd.GetFuncConfig, Cmd.SetFuncConfig]).toEqual([0x44, 0x45]);
    expect([FunctionId.Line, FunctionId.Track]).toEqual([4, 5]);
  });

  it("parseInfo reads 5 function ids", () => {
    expect(parseInfo(fromHex(frameVector("get_info_response_m6").payload_hex)).functionIds).toEqual([1, 2, 3, 4, 5]);
  });

  it("encodeGetFuncConfig", () => {
    expect(frameVector("get_func_config_line_request").cmd).toBe(Cmd.GetFuncConfig);
    expect(toHex(encodeGetFuncConfig(FunctionId.Line))).toBe(frameVector("get_func_config_line_request").payload_hex);
  });

  it("func config vectors round trip", () => {
    const l = pv.line_config_default;
    const line = parseFuncConfig(fromHex(frameVector("func_config_line_default").payload_hex));
    expect(line).toEqual({
      functionId: 4,
      line: {
        mode: l.mode, threshold: l.threshold, roiPct: l.roi_pct, bands: l.bands, minWidth: l.min_width,
        maxWidth: l.max_width, colorTolerance: l.color_tolerance,
      },
    });
    expect(toHex(encodeFuncConfig(line))).toBe(frameVector("func_config_line_default").payload_hex);

    const t = pv.track_config_set;
    const track = parseFuncConfig(fromHex(frameVector("set_func_config_track_request").payload_hex));
    expect(track).toEqual({
      functionId: 5,
      track: { threshold: t.threshold, adapt: t.adapt, reacquire: t.reacquire !== 0, scale: t.scale !== 0 },
    });
    expect(toHex(encodeFuncConfig(track))).toBe(frameVector("set_func_config_track_request").payload_hex);
  });

  it("encodeFuncConfig writes reserved bytes as 0", () => {
    const b = encodeFuncConfig({ functionId: 5, track: { threshold: 50, adapt: 10, reacquire: true, scale: true } });
    expect(toHex(b)).toBe("0501320a010100000000");
  });

  it("parseFuncConfig rejects wrong fn/version/length", () => {
    const line = fromHex(frameVector("func_config_line_default").payload_hex);
    expect(() => parseFuncConfig(line.slice(0, 9))).toThrow();
    expect(() => parseFuncConfig(new Uint8Array([...line, 0]))).toThrow();
    const v2 = line.slice(); v2[1] = 2;
    expect(() => parseFuncConfig(v2)).toThrow();
    const fn1 = line.slice(); fn1[0] = 1;
    expect(() => parseFuncConfig(fn1)).toThrow();
    const fn6 = line.slice(); fn6[0] = 6;
    expect(() => parseFuncConfig(fn6)).toThrow();
    const track = fromHex(frameVector("set_func_config_track_request").payload_hex);
    const tv0 = track.slice(); tv0[1] = 0;
    expect(() => parseFuncConfig(tv0)).toThrow();
  });

  it("parseFuncConfig does not clamp", () => {
    const p = fromHex(frameVector("func_config_line_default").payload_hex);
    p[4] = 150;
    expect(parseFuncConfig(p)).toMatchObject({ functionId: 4, line: { roiPct: 150 } });
  });

  it("encodeLearn with id", () => {
    const v = pv.learn_request_with_id;
    const rect = { x: v.x, y: v.y, width: v.width, height: v.height };
    expect(toHex(encodeLearn(v.function_id, rect, v.id))).toBe(frameVector("learn_request_with_id").payload_hex);
    expect(encodeLearn(v.function_id, rect)).toHaveLength(9);
  });

  it("parseResults decodes line extra", () => {
    const r = parseResults(fromHex(frameVector("results_line_one").payload_hex));
    const e = pv.results_line_one.results[0];
    expect(r.frameId).toBe(77);
    expect(r.functionId).toBe(FunctionId.Line);
    expect(r.results[0]).toMatchObject({ id: 1, x: e.x, y: e.y, width: e.width, height: e.height, confidence: 88 });
    expect(r.results[0].line).toEqual({
      angleDeg: -8.49, offsetX: 20, start: [180, 231], end: [160, 97], branch: false, cross: true,
    });
    expect(r.results[0].extra).toBeUndefined();
  });

  it("parseResults accepts unknown extra lengths", () => {
    // function 9, one record with extra_len 5
    const p = new Uint8Array([1, 0, 0, 0, 9, 1, 1, 0, 10, 0, 20, 0, 4, 0, 4, 0, 50, 5, 1, 2, 3, 4, 5]);
    const r = parseResults(p);
    expect(r.results).toHaveLength(1);
    expect(r.results[0]).toEqual({ id: 1, x: 10, y: 20, width: 4, height: 4, confidence: 50 });
    // an 18-byte extra on the line function and a 14-byte extra on AprilTag are skipped too
    const line18 = new Uint8Array([1, 0, 0, 0, 4, 1, 1, 0, 10, 0, 20, 0, 4, 0, 4, 0, 50, 18, ...new Array(18).fill(0)]);
    expect(parseResults(line18).results[0]).toEqual({ id: 1, x: 10, y: 20, width: 4, height: 4, confidence: 50 });
    const tag14 = new Uint8Array([1, 0, 0, 0, 2, 1, 1, 0, 10, 0, 20, 0, 4, 0, 4, 0, 50, 14, ...new Array(14).fill(0)]);
    expect(parseResults(tag14).results[0]).toEqual({ id: 1, x: 10, y: 20, width: 4, height: 4, confidence: 50 });
  });

  it("parseResults rejects extra_len 19", () => {
    const p = new Uint8Array([1, 0, 0, 0, 9, 1, 1, 0, 10, 0, 20, 0, 4, 0, 4, 0, 50, 19, ...new Array(19).fill(0)]);
    expect(() => parseResults(p)).toThrow();
  });
});

describe("M7: QR, shapes and motion", () => {
  it("command and function ids", () => {
    expect([Cmd.GetResultData, Cmd.GetLearnedData]).toEqual([0x21, 0x35]);
    expect([responseOf(Cmd.GetResultData), responseOf(Cmd.GetLearnedData)]).toEqual([0xa1, 0xb5]);
    expect([FunctionId.Qr, FunctionId.Shape, FunctionId.Motion]).toEqual([6, 11, 12]);
  });

  it("parseInfo reads m7 function ids", () => {
    expect(parseInfo(fromHex(frameVector("get_info_response_m7").payload_hex)).functionIds).toEqual([1, 2, 3, 4, 5, 6, 8, 11, 12]);
  });

  it("shape and motion func config vectors round trip", () => {
    expect(toHex(encodeGetFuncConfig(FunctionId.Shape))).toBe(frameVector("get_func_config_shape_request").payload_hex);
    const s = pv.shape_config_default;
    const shape = parseFuncConfig(fromHex(frameVector("func_config_shape_default").payload_hex));
    expect(shape).toEqual({
      functionId: 11, shape: { mode: s.mode, threshold: s.threshold, minSize: s.min_size, maxResults: s.max_results },
    });
    expect(toHex(encodeFuncConfig(shape))).toBe(frameVector("func_config_shape_default").payload_hex);

    const motionDefault = parseFuncConfig(fromHex(frameVector("func_config_motion_default").payload_hex));
    expect(motionDefault).toEqual({ functionId: 12, motion: { threshold: 25, minSize: 24, maxResults: 4 } });
    expect(toHex(encodeFuncConfig(motionDefault))).toBe(frameVector("func_config_motion_default").payload_hex);

    const m = pv.motion_config_set;
    const motion = parseFuncConfig(fromHex(frameVector("set_func_config_motion_request").payload_hex));
    expect(motion).toEqual({ functionId: 12, motion: { threshold: m.threshold, minSize: m.min_size, maxResults: m.max_results } });
    expect(motion).toEqual({ functionId: 12, motion: { threshold: 15, minSize: 48, maxResults: 2 } });
    expect(toHex(encodeFuncConfig(motion))).toBe(frameVector("set_func_config_motion_request").payload_hex);
  });

  it("encodeFuncConfig writes shape/motion reserved bytes as 0", () => {
    expect(toHex(encodeFuncConfig({ functionId: 11, shape: { mode: 1, threshold: 90, minSize: 40, maxResults: 8 } })))
      .toBe("0b01015a280800000000");
    expect(toHex(encodeFuncConfig({ functionId: 12, motion: { threshold: 30, minSize: 16, maxResults: 3 } })))
      .toBe("0c011e10030000000000");
  });

  it("parseFuncConfig rejects wrong shape/motion version and length", () => {
    for (const name of ["func_config_shape_default", "func_config_motion_default"]) {
      const p = fromHex(frameVector(name).payload_hex);
      expect(() => parseFuncConfig(p.slice(0, 9))).toThrow();
      expect(() => parseFuncConfig(new Uint8Array([...p, 0]))).toThrow();
      const v2 = p.slice(); v2[1] = 2;
      expect(() => parseFuncConfig(v2)).toThrow(/layout_version/);
      const v0 = p.slice(); v0[1] = 0;
      expect(() => parseFuncConfig(v0)).toThrow(/layout_version/);
    }
    const qr = fromHex(frameVector("func_config_shape_default").payload_hex);
    qr[0] = FunctionId.Qr;
    expect(() => parseFuncConfig(qr)).toThrow();
  });

  it("parseResults decodes qr corners", () => {
    const r = parseResults(fromHex(frameVector("results_qr_two").payload_hex));
    const e = pv.results_qr_two;
    expect(r.frameId).toBe(300);
    expect(r.functionId).toBe(FunctionId.Qr);
    expect(r.results.map((x) => [x.id, x.x, x.y, x.width, x.height, x.confidence])).toEqual(
      e.results.map((x) => [x.id, x.x, x.y, x.width, x.height, undefined]));
    expect(r.results[0].extra).toEqual({ rotationDeg: 0, corners: e.results[0].corners_extra?.corners });
    expect(r.results[0].extra?.corners[1]).toEqual([160, 60]);
    expect(r.results[1].extra).toEqual({ rotationDeg: -90, corners: [[220, 200], [220, 160], [260, 160], [260, 200]] });
    for (const x of r.results) expect(x.line).toBeUndefined();
  });

  it("result data vectors", () => {
    const hello = parseResultData(fromHex(frameVector("result_data_hello").payload_hex));
    const h = pv.result_data_hello;
    expect(hello).toEqual({ frameId: h.frame_id, index: h.index, truncated: false, totalLength: h.total_len, data: fromHex(h.data_hex) });
    expect(new TextDecoder().decode(hello.data)).toBe("HELLO");

    const t = parseResultData(fromHex(frameVector("result_data_truncated").payload_hex));
    expect(t.frameId).toBe(301);
    expect(t.index).toBe(1);
    expect(t.truncated).toBe(true);
    expect(t.totalLength).toBe(300);
    expect(t.data.length).toBe(128);
    expect(t.data[127]).toBe(127);

    const p = fromHex(frameVector("result_data_hello").payload_hex);
    expect(() => parseResultData(p.slice(0, p.length - 1))).toThrow();
    expect(() => parseResultData(new Uint8Array([...p, 0]))).toThrow();
    expect(() => parseResultData(p.slice(0, 8))).toThrow();
    const big = new Uint8Array(9 + 129); big[8] = 129;
    expect(() => parseResultData(big)).toThrow();
  });

  it("learned data vector", () => {
    const d = parseLearnedData(fromHex(frameVector("learned_data_qr").payload_hex));
    const l = pv.learned_data_qr;
    expect(d).toEqual({ functionId: l.function_id, id: l.id, truncated: false, totalLength: l.total_len, data: fromHex(l.data_hex) });
    expect(new TextDecoder().decode(d.data)).toBe("LEFT");
    const p = fromHex(frameVector("learned_data_qr").payload_hex);
    expect(() => parseLearnedData(p.slice(0, p.length - 1))).toThrow();
    expect(() => parseLearnedData(new Uint8Array([...p, 0]))).toThrow();
    expect(() => parseLearnedData(p.slice(0, 6))).toThrow();
  });

  it("encodeGetResultData and encodeGetLearnedData equal the vectors", () => {
    expect(frameVector("get_result_data_request").cmd).toBe(Cmd.GetResultData);
    expect(toHex(encodeGetResultData(0))).toBe(frameVector("get_result_data_request").payload_hex);
    expect(frameVector("get_learned_data_request").cmd).toBe(Cmd.GetLearnedData);
    expect(toHex(encodeGetLearnedData(FunctionId.Qr, 2))).toBe(frameVector("get_learned_data_request").payload_hex);
    expect(frameVector("result_data_hello").cmd).toBe(responseOf(Cmd.GetResultData));
    expect(frameVector("learned_data_qr").cmd).toBe(responseOf(Cmd.GetLearnedData));
  });

  it("Shape ids match the protocol table", () => {
    expect(Shape).toEqual({ Circle: 1, Triangle: 2, Square: 3, Rect: 4 });
  });
});
