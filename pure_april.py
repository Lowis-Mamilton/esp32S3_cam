import cv2
import numpy as np
import serial
import struct
import time
from pupil_apriltags import Detector

PORT = "COM11"
BAUD = 921600

# 以前電腦能框到時的方向：依你當時為準
USE_FLIP = True          # 以前有左右翻就 True
USE_ROTATE_180 = False   # 以前有轉 180 就 True

# 除錯用：True = 電腦也顯示（應能再框到）；正式可改 False
SHOW_WINDOW = True

detector = Detector(
    families="tag36h11",
    nthreads=2,
    quad_decimate=1.0,
    refine_edges=1,
    decode_sharpening=0.25,
)

def read_exact(ser, n):
    data = bytearray()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)

def read_frame(ser):
    while True:
        b = ser.read(1)
        if not b:
            return None
        if b[0] != 0xAA:
            continue
        b2 = ser.read(1)
        if not b2 or b2[0] != 0x55:
            continue
        break
    header = read_exact(ser, 4)
    if not header:
        return None
    length = struct.unpack("<I", header)[0]
    if length < 100 or length > 300000:
        return None
    jpeg = read_exact(ser, length)
    if not jpeg:
        return None
    return cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)

def rot_deg(corners):
    c = np.array(corners, dtype=np.float32)
    return float(np.degrees(np.arctan2(c[1][1] - c[0][1], c[1][0] - c[0][0])))

def main():
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(2)
    ser.reset_input_buffer()
    print("判斷在電腦，顯示在 HSD | Q 結束（若有開視窗）")

    while True:
        frame = read_frame(ser)
        if frame is None:
            continue

        # ===== 與「以前能框到」相同的前處理 =====
        if USE_ROTATE_180:
            frame = cv2.rotate(frame, cv2.ROTATE_180)
        if USE_FLIP:
            frame = cv2.flip(frame, 1)

        if frame.shape[1] != 320 or frame.shape[0] != 240:
            frame = cv2.resize(frame, (320, 240))

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        tags = detector.detect(gray)

        view = frame.copy()
        line = "#TAG,NONE\n"

        if tags:
            t = max(tags, key=lambda x: getattr(x, "decision_margin", 0))
            c = t.corners
            cx, cy = t.center
            r = rot_deg(c)

            def ci(v, m):
                return int(max(0, min(m, round(v))))

            # 送給 HSD 的座標（與這張 frame 一致）
            line = (
                f"#TAG,{int(t.tag_id)},{ci(cx,319)},{ci(cy,239)},"
                f"{ci(c[0][0],319)},{ci(c[0][1],239)},"
                f"{ci(c[1][0],319)},{ci(c[1][1],239)},"
                f"{ci(c[2][0],319)},{ci(c[2][1],239)},"
                f"{ci(c[3][0],319)},{ci(c[3][1],239)},"
                f"{r:.1f}\n"
            )
            print(line.strip())

            if SHOW_WINDOW:
                pts = c.astype(np.int32).reshape((-1, 1, 2))
                cv2.polylines(view, [pts], True, (0, 255, 0), 2)
                cv2.circle(view, (ci(cx,319), ci(cy,239)), 4, (0, 0, 255), -1)
                cv2.putText(
                    view, f"ID:{int(t.tag_id)} {r:.0f}d",
                    (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1
                )

        ser.write(line.encode("ascii"))
        ser.flush()

        if SHOW_WINDOW:
            cv2.imshow("PC check (optional)", view)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

    ser.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()